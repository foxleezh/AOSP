#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

void parse_error(struct parse_state *state, const char *fmt, ...)
{
    va_list ap;
    char buf[128];
    int off;

    snprintf(buf, sizeof(buf), "%s: %d: ", state->filename, state->line);
    buf[127] = 0;
    off = strlen(buf);

    va_start(ap, fmt);
    vsnprintf(buf + off, 128 - off, fmt, ap);
    va_end(ap);
    buf[127] = 0;
    LOG(ERROR) << buf;
}

int next_token(struct parse_state *state)
{
    char *x = state->ptr;
    char *s;

    if (state->nexttoken) {
        int t = state->nexttoken;
        state->nexttoken = 0;
        return t;
    }

    for (;;) {
        switch (*x) {
        case 0:
            state->ptr = x;
            return T_EOF;
        case '\n':
            x++;
            state->ptr = x;
            return T_NEWLINE;
        case ' ':
        case '\t':
        case '\r':
            x++;
            continue;
        case '#':
            while (*x && (*x != '\n')) x++;
            if (*x == '\n') {
                state->ptr = x+1;
                return T_NEWLINE;
            } else {
                state->ptr = x;
                return T_EOF;
            }
        default:
            goto text;
        }
    }

textdone:
    state->ptr = x;
    *s = 0;
    return T_TEXT;
text:
    state->text = s = x;
textresume:
    for (;;) {
        switch (*x) {
        case 0:
            goto textdone;
        case ' ':
        case '\t':
        case '\r':
            x++;
            goto textdone;
        case '\n':
            state->nexttoken = T_NEWLINE;
            x++;
            goto textdone;
        case '"':
            x++;
            for (;;) {
                switch (*x) {
                case 0:
                        /* unterminated quoted thing */
                    state->ptr = x;
                    return T_EOF;
                case '"':
                    x++;
                    goto textresume;
                default:
                    *s++ = *x++;
                }
            }
            break;
        case '\\':
            x++;
            switch (*x) {
            case 0:
                goto textdone;
            case 'n':
                *s++ = '\n';
                break;
            case 'r':
                *s++ = '\r';
                break;
            case 't':
                *s++ = '\t';
                break;
            case '\\':
                *s++ = '\\';
                break;
            case '\r':
                    /* \ <cr> <lf> -> line continuation */
                if (x[1] != '\n') {
                    x++;
                    continue;
                }
            case '\n':
                    /* \ <lf> -> line continuation */
                state->line++;
                x++;
                    /* eat any extra whitespace */
                while((*x == ' ') || (*x == '\t')) x++;
                continue;
            default:
                    /* unknown escape -- just copy */
                *s++ = *x++;
            }
            continue;
        default:
            *s++ = *x++;
        }
    }
    return T_EOF;
}
