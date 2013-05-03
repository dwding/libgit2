/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "git2.h"
#include "buffer.h"
#include "netops.h"

#include <libssh2.h>

#define OWNING_SUBTRANSPORT(s) ((ssh_subtransport *)(s)->parent.subtransport)

static const char prefix_ssh[] = "ssh://";
static const char prefix_git[] = "git@";
static const char cmd_uploadpack[] = "git-upload-pack";
static const char cmd_receivepack[] = "git-receive-pack";

typedef struct {
	git_smart_subtransport_stream parent;
	gitno_socket socket;
	const char *cmd;
	char *url;
	unsigned sent_command : 1;
} ssh_stream;

typedef struct {
	git_smart_subtransport parent;
	git_transport *owner;
	ssh_stream *current_stream;
} ssh_subtransport;

/*
 * Create a git protocol request.
 *
 * For example: 0035git-upload-pack /libgit2/libgit2\0
 */
static int gen_proto(git_buf *request, const char *cmd, const char *url)
{
	char *delim, *repo;
	char host[] = "host=";
	size_t len;
	
	delim = strchr(url, '/');
	if (delim == NULL) {
		giterr_set(GITERR_NET, "Malformed URL");
		return -1;
	}
	
	repo = delim;
	
	delim = strchr(url, ':');
	if (delim == NULL)
		delim = strchr(url, '/');
	
	len = 4 + strlen(cmd) + 1 + strlen(repo) + 1 + strlen(host) + (delim - url) + 1;
	
	git_buf_grow(request, len);
	git_buf_printf(request, "%04x%s %s%c%s",
				   (unsigned int)(len & 0x0FFFF), cmd, repo, 0, host);
	git_buf_put(request, url, delim - url);
	git_buf_putc(request, '\0');
	
	if (git_buf_oom(request))
		return -1;
	
	return 0;
}

static int send_command(ssh_stream *s)
{
	int error;
	git_buf request = GIT_BUF_INIT;
	
	error = gen_proto(&request, s->cmd, s->url);
	if (error < 0)
		goto cleanup;
	
	/* It looks like negative values are errors here, and positive values
	 * are the number of bytes sent. */
	error = gitno_send(&s->socket, request.ptr, request.size, 0);
	
	if (error >= 0)
		s->sent_command = 1;
	
cleanup:
	git_buf_free(&request);
	return error;
}

static int ssh_stream_read(
	git_smart_subtransport_stream *stream,
	char *buffer,
	size_t buf_size,
	size_t *bytes_read)
{
	ssh_stream *s = (ssh_stream *)stream;
	gitno_buffer buf;
	
	*bytes_read = 0;
	
	if (!s->sent_command && send_command(s) < 0)
		return -1;
	
	gitno_buffer_setup(&s->socket, &buf, buffer, buf_size);
	
	if (gitno_recv(&buf) < 0)
		return -1;
	
	*bytes_read = buf.offset;
	
	return 0;
}

static int ssh_stream_write(
	git_smart_subtransport_stream *stream,
	const char *buffer,
	size_t len)
{
	ssh_stream *s = (ssh_stream *)stream;
	
	if (!s->sent_command && send_command(s) < 0)
		return -1;
	
	return gitno_send(&s->socket, buffer, len, 0);
}

static void ssh_stream_free(git_smart_subtransport_stream *stream)
{
	ssh_stream *s = (ssh_stream *)stream;
	ssh_subtransport *t = OWNING_SUBTRANSPORT(s);
	int ret;
	
	GIT_UNUSED(ret);
	
	t->current_stream = NULL;
	
	if (s->socket.socket) {
		ret = gitno_close(&s->socket);
		assert(!ret);
	}
	
	git__free(s->url);
	git__free(s);
}

static int ssh_stream_alloc(
	ssh_subtransport *t,
	const char *url,
	const char *cmd,
	git_smart_subtransport_stream **stream)
{
	ssh_stream *s;
	
	if (!stream)
		return -1;
	
	s = git__calloc(sizeof(ssh_stream), 1);
	GITERR_CHECK_ALLOC(s);
	
	s->parent.subtransport = &t->parent;
	s->parent.read = ssh_stream_read;
	s->parent.write = ssh_stream_write;
	s->parent.free = ssh_stream_free;
	
	s->cmd = cmd;
	s->url = git__strdup(url);
	
	if (!s->url) {
		git__free(s);
		return -1;
	}
	
	*stream = &s->parent;
	return 0;
}

/* Temp */
static int gitssh_extract_url_parts(
	char **host,
	char **username,
	char **path,
	const char *url)
{
	char *colon, *at;
	const char *start;
    
    colon = strchr(url, ':');
	at = strchr(url, '@');
	
	if (colon == NULL) {
		giterr_set(GITERR_NET, "Malformed URL: missing :");
		return -1;
	}
	
	start = url;
	if (at) {
		start = at+1;
		*username = git__substrdup(url, at - url);
	} else {
		*username = "git";
	}
	
	*host = git__substrdup(start, colon - start);
	*path = colon+1;
	
	return 0;
}

static int _git_uploadpack_ls(
	ssh_subtransport *t,
	const char *url,
	git_smart_subtransport_stream **stream)
{
	char *host, *port, *user=NULL, *pass=NULL;
	ssh_stream *s;
	
	*stream = NULL;
	
	if (!git__prefixcmp(url, prefix_ssh))
		url += strlen(prefix_ssh);
	
	if (ssh_stream_alloc(t, url, cmd_uploadpack, stream) < 0)
		return -1;
	
	s = (ssh_stream *)*stream;
	
	if (gitno_extract_url_parts(&host, &port, &user, &pass, url, GIT_DEFAULT_PORT) < 0)
		goto on_error;
	
	if (gitno_connect(&s->socket, host, port, 0) < 0)
		goto on_error;
	
	t->current_stream = s;
	git__free(host);
	git__free(port);
	git__free(user);
	git__free(pass);
	return 0;
	
on_error:
	if (*stream)
		ssh_stream_free(*stream);
	
	git__free(host);
	git__free(port);
	return -1;
}

static int _git_uploadpack(
	ssh_subtransport *t,
	const char *url,
	git_smart_subtransport_stream **stream)
{
	GIT_UNUSED(url);
	
	if (t->current_stream) {
		*stream = &t->current_stream->parent;
		return 0;
	}
	
	giterr_set(GITERR_NET, "Must call UPLOADPACK_LS before UPLOADPACK");
	return -1;
}

static int _git_receivepack_ls(
	ssh_subtransport *t,
	const char *url,
	git_smart_subtransport_stream **stream)
{
	char *host, *path, *user=NULL;
	ssh_stream *s;
	
	*stream = NULL;
	if (ssh_stream_alloc(t, url, cmd_receivepack, stream) < 0)
		return -1;
	
	s = (ssh_stream *)*stream;
	
	if (gitssh_extract_url_parts(&host, &user, &path, url) < 0)
		goto on_error;
	
	if (gitno_connect(&s->socket, host, "22", 0) < 0)
		goto on_error;
	
	LIBSSH2_SESSION* session = libssh2_session_init();
    if (!session)
        goto on_error;
	
    int rc = 0;
    do {
        rc = libssh2_session_startup(session, s->socket.socket);
    } while (LIBSSH2_ERROR_EAGAIN == rc || LIBSSH2_ERROR_TIMEOUT == rc);
	
	if (0 != rc) {
        goto on_error;
    }
	
	libssh2_trace(session, 0x1FF);
	libssh2_session_set_blocking(session, 1);
	
    do {
        rc = libssh2_userauth_publickey_fromfile_ex(
            session, 
            user,
			strlen(user),
            NULL,
			"/Users/bradfordmorgan/.ssh/id_rsa",
            NULL
        );
    } while (LIBSSH2_ERROR_EAGAIN == rc || LIBSSH2_ERROR_TIMEOUT == rc);
	
    if (0 != rc) {
        goto on_error;
    }
	
	LIBSSH2_CHANNEL* channel = NULL;
    do {
        channel = libssh2_channel_open_session(session);
    } while (LIBSSH2_ERROR_EAGAIN == rc || LIBSSH2_ERROR_TIMEOUT == rc);
	
	if (!channel) {
        goto on_error;
    }
	
	libssh2_channel_set_blocking(channel, 1);
	
	t->current_stream = s;
	git__free(host);
	return 0;
	
on_error:
	if (*stream)
		ssh_stream_free(*stream);
	
	git__free(host);
	git__free(path);
	return -1;
}

static int _git_receivepack(
	ssh_subtransport *t,
	const char *url,
	git_smart_subtransport_stream **stream)
{
	GIT_UNUSED(url);
	
	if (t->current_stream) {
		*stream = &t->current_stream->parent;
		return 0;
	}
	
	giterr_set(GITERR_NET, "Must call RECEIVEPACK_LS before RECEIVEPACK");
	return -1;
}

static int _git_action(
	git_smart_subtransport_stream **stream,
	git_smart_subtransport *subtransport,
	const char *url,
	git_smart_service_t action)
{
	ssh_subtransport *t = (ssh_subtransport *) subtransport;
	
	switch (action) {
		case GIT_SERVICE_UPLOADPACK_LS:
			return _git_uploadpack_ls(t, url, stream);
			
		case GIT_SERVICE_UPLOADPACK:
			return _git_uploadpack(t, url, stream);
			
		case GIT_SERVICE_RECEIVEPACK_LS:
			return _git_receivepack_ls(t, url, stream);
			
		case GIT_SERVICE_RECEIVEPACK:
			return _git_receivepack(t, url, stream);
	}
	
	*stream = NULL;
	return -1;
}

static int _git_close(git_smart_subtransport *subtransport)
{
	ssh_subtransport *t = (ssh_subtransport *) subtransport;
	
	assert(!t->current_stream);
	
	GIT_UNUSED(t);
	
	return 0;
}

static void _git_free(git_smart_subtransport *subtransport)
{
	ssh_subtransport *t = (ssh_subtransport *) subtransport;
	
	assert(!t->current_stream);
	
	git__free(t);
}

int git_smart_subtransport_ssh(git_smart_subtransport **out, git_transport *owner)
{
	ssh_subtransport *t;
	
	if (!out)
		return -1;
	
	t = git__calloc(sizeof(ssh_subtransport), 1);
	GITERR_CHECK_ALLOC(t);
	
	t->owner = owner;
	t->parent.action = _git_action;
	t->parent.close = _git_close;
	t->parent.free = _git_free;
	
	*out = (git_smart_subtransport *) t;
	return 0;
}
