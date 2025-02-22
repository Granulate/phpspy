#include "phpspy.h"

int output_fd = -1;
typedef struct event_handler_fout_udata_s {
    char *buf;
    char *cur;
    size_t buf_size;
    size_t rem;
} event_handler_fout_udata_t;

static int event_handler_fout_write(event_handler_fout_udata_t *udata);
static int event_handler_fout_snprintf(char **s, size_t *n, size_t *ret_len, int repl_delim, const char *fmt, ...);

int event_handler_fout(struct trace_context_s *context, int event_type) {
    int rv;
    size_t len;
    trace_frame_t *frame;
    trace_request_t *request;
    event_handler_fout_udata_t *udata;
    struct timeval tv;

    udata = (event_handler_fout_udata_t*)context->event_udata;
    if (!udata && event_type != PHPSPY_TRACE_EVENT_INIT) {
        return PHPSPY_ERR;
    }
    len = 0;
    switch (event_type) {
        case PHPSPY_TRACE_EVENT_INIT:
            udata = calloc(1, sizeof(event_handler_fout_udata_t));
            udata->buf_size = opt_fout_buffer_size + 1; /* + 1 for null char */

            /* When not in pgrep mode can share the same buffer because there is only a single thread */
            if (!in_pgrep_mode) {
                udata->buf = malloc(udata->buf_size);
            }

            context->event_udata = udata;
            break;
        case PHPSPY_TRACE_EVENT_STACK_BEGIN:
            if (in_pgrep_mode) {
                udata->buf = malloc(udata->buf_size);
            }

            udata->cur = udata->buf;
            udata->cur[0] = '\0';
            udata->rem = udata->buf_size;
            break;
        case PHPSPY_TRACE_EVENT_FRAME:
            frame = &context->event.frame;
            try(rv, event_handler_fout_snprintf(
                &udata->cur,
                &udata->rem,
                &len,
                1,
                "%d %.*s%s%.*s %.*s:%d",
                frame->depth,
                (int)frame->loc.class_len, frame->loc.class,
                frame->loc.class_len > 0 ? "::" : "",
                (int)frame->loc.func_len, frame->loc.func,
                (int)frame->loc.file_len, frame->loc.file,
                frame->loc.lineno
            ));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
            break;
        case PHPSPY_TRACE_EVENT_VARPEEK:
            try(rv, event_handler_fout_snprintf(
                &udata->cur,
                &udata->rem,
                &len,
                1,
                "# varpeek %s@%s = %s",
                context->event.varpeek.var->name,
                context->event.varpeek.entry->filename_lineno,
                context->event.varpeek.zval_str
            ));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
            break;
        case PHPSPY_TRACE_EVENT_GLOPEEK:
            try(rv, event_handler_fout_snprintf(
                &udata->cur,
                &udata->rem,
                &len,
                1,
                "# glopeek %s = %s",
                context->event.glopeek.gentry->key,
                context->event.glopeek.zval_str
            ));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
            break;
        case PHPSPY_TRACE_EVENT_REQUEST:
            request = &context->event.request;
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 1, "# uri = %s", request->uri));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 1, "# path = %s", request->path));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 1, "# qstring = %s", request->qstring));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 1, "# cookie = %s", request->cookie));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 1, "# ts = %f", request->ts));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
            break;
        case PHPSPY_TRACE_EVENT_MEM:
            try(rv, event_handler_fout_snprintf(
                &udata->cur,
                &udata->rem,
                &len,
                1,
                "# mem %lu %lu",
                (uint64_t)context->event.mem.size,
                (uint64_t)context->event.mem.peak
            ));
            try(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
            break;
        case PHPSPY_TRACE_EVENT_STACK_END:
            if (udata->cur == udata->buf) {
                /* buffer is empty */
                break;
            }
            if (opt_filter_re) {
                rv = regexec(opt_filter_re, udata->buf, 0, NULL, 0);
                if (opt_filter_negate == 0 && rv != 0) break;
                if (opt_filter_negate != 0 && rv == 0) break;
            }
            do {
                if (opt_verbose_fields_ts) {
                    gettimeofday(&tv, NULL);
                    try_break(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 1, "# trace_ts = %f", (double)(tv.tv_sec + tv.tv_usec / 1000000.0)));
                    try_break(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
                }
                if (opt_verbose_fields_pid) {
                    try_break(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 1, "# pid = %d", context->target.pid));
                    try_break(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
                }
                if (opt_verbose_fields_phpv) {
                    try_break(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 1, "# phpv = %s", context->target.phpv));
                    try_break(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_frame_delim));
                }
                try_break(rv, event_handler_fout_snprintf(&udata->cur, &udata->rem, &len, 0, "%c", opt_trace_delim));
            } while (0);
            try(rv, event_handler_fout_write(udata));
            break;
        case PHPSPY_TRACE_EVENT_DEINIT:
            if (!in_pgrep_mode) {
                free(udata->buf); /* Freed in pgrep.c: drain_pipe_to_file */
            }
            free(udata);
            break;
    }
    return PHPSPY_OK;
}

static int event_handler_fout_write(event_handler_fout_udata_t *udata) {
    ssize_t write_len;
    write_len = (udata->cur - udata->buf);

    if (write_len < 1) {
        /* nothing to write */
    } else {
        if (in_pgrep_mode) {
            return pgrep_mode_output_write(udata->buf, write_len);
        }

        if (write(output_fd, udata->buf, write_len) != write_len) {
            log_error("event_handler_fout: Write failed (%s)\n", errno != 0 ? strerror(errno) : "partial");
            return PHPSPY_ERR;
        }
    }

    return PHPSPY_OK;
}

static int event_handler_fout_snprintf(char **s, size_t *n, size_t *ret_len, int repl_delim, const char *fmt, ...) {
    int len, i;
    va_list vl;
    char *c;

    va_start(vl, fmt);
    len = vsnprintf(*s, *n, fmt, vl);
    va_end(vl);

    if (len < 0 || (size_t)len >= *n) {
        log_error("event_handler_fout_snprintf: Not enough space in buffer; truncating\n");
        return PHPSPY_ERR | PHPSPY_ERR_BUF_FULL;
    }

    if (repl_delim) {
        for (i = 0; i < len; i++) { /* TODO optimize */
            c = *s + i;
            if (*c == opt_trace_delim || *c == opt_frame_delim) {
                *c = '?';
            }
        }
    }

    *s += len;
    *n -= len;
    *ret_len = (size_t)len;

    return PHPSPY_OK;
}

int event_handler_fout_open(int *fd) {
    int tfd;
    if (strcmp(opt_path_output, STDOUT_OUTPUT) == 0) {
        tfd = dup(STDOUT_FILENO);
        if (tfd < 0) {
            perror("event_handler_fout_open: dup");
            return PHPSPY_ERR;
        }
    } else {
        tfd = open(opt_path_output, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (tfd < 0) {
            perror("event_handler_fout_open: open");
            return PHPSPY_ERR;
        }
    }
    *fd = tfd;
    return PHPSPY_OK;
}


int init_output_fd(void) {
    return event_handler_fout_open(&output_fd);
}

void deinit_output_fd(void){
    if (output_fd > 0) {
        close(output_fd);
        output_fd = -1;
    }
}
