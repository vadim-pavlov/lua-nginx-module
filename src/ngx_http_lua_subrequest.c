#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"

#include "ngx_http_lua_subrequest.h"
#include "ngx_http_lua_util.h"
#include "ngx_http_lua_ctx.h"
#include "ngx_http_lua_contentby.h"


#define NGX_HTTP_LUA_SHARE_ALL_VARS     0x01
#define NGX_HTTP_LUA_COPY_ALL_VARS      0x02


#define ngx_http_lua_method_name(m) { sizeof(m) - 1, (u_char *) m " " }

ngx_str_t  ngx_http_lua_get_method = ngx_http_lua_method_name("GET");
ngx_str_t  ngx_http_lua_put_method = ngx_http_lua_method_name("PUT");
ngx_str_t  ngx_http_lua_post_method = ngx_http_lua_method_name("POST");
ngx_str_t  ngx_http_lua_head_method = ngx_http_lua_method_name("HEAD");
ngx_str_t  ngx_http_lua_delete_method =
        ngx_http_lua_method_name("DELETE");
ngx_str_t  ngx_http_lua_options_method =
        ngx_http_lua_method_name("OPTIONS");

static ngx_str_t  ngx_http_lua_content_length_header_key =
        ngx_string("Content-Length");

static ngx_int_t ngx_http_lua_sub_request_set_content_length_header(
        ngx_http_request_t *r, off_t len);
static ngx_int_t ngx_http_lua_sub_request_set_extra_headers(
        ngx_http_request_t *r, ngx_array_t *extra_headers);
static ngx_int_t ngx_http_lua_sub_request_copy_parent_headers(
        ngx_http_request_t *r);
static ngx_int_t ngx_http_lua_sub_request_set_headers(ngx_http_request_t *r,
        off_t content_len, ngx_array_t *extra_headers)

static ngx_int_t ngx_http_lua_adjust_subrequest(ngx_http_request_t *sr,
    ngx_uint_t method, ngx_array_t *extra_headers, ngx_http_request_body_t *body,
    unsigned vars_action, ngx_array_t *extra_vars);
static int ngx_http_lua_ngx_location_capture(lua_State *L);
static int ngx_http_lua_ngx_location_capture_multi(lua_State *L);
static void ngx_http_lua_process_vars_option(ngx_http_request_t *r,
    lua_State *L, int table, ngx_array_t **varsp);
static void ngx_http_lua_process_extra_headers_option(ngx_http_request_t *r,
    lua_State *L, ngx_array_t **varsp);
static ngx_int_t ngx_http_lua_subrequest_add_extra_vars(ngx_http_request_t *r,
    ngx_array_t *extra_vars);


/* ngx.location.capture is just a thin wrapper around
 * ngx.location.capture_multi */
static int
ngx_http_lua_ngx_location_capture(lua_State *L)
{
    int                 n;

    n = lua_gettop(L);

    if (n != 1 && n != 2) {
        return luaL_error(L, "expecting one or two arguments");
    }

    lua_createtable(L, n, 0); /* uri opts? table  */
    lua_insert(L, 1); /* table uri opts? */
    if (n == 1) { /* table uri */
        lua_rawseti(L, 1, 1); /* table */

    } else { /* table uri opts */
        lua_rawseti(L, 1, 2); /* table uri */
        lua_rawseti(L, 1, 1); /* table */
    }

    lua_createtable(L, 1, 0); /* table table' */
    lua_insert(L, 1);   /* table' table */
    lua_rawseti(L, 1, 1); /* table' */

    return ngx_http_lua_ngx_location_capture_multi(L);
}


static int
ngx_http_lua_ngx_location_capture_multi(lua_State *L)
{
    ngx_http_request_t              *r;
    ngx_http_request_t              *sr; /* subrequest object */
    ngx_http_post_subrequest_t      *psr;
    ngx_http_lua_ctx_t              *sr_ctx;
    ngx_http_lua_ctx_t              *ctx;
    ngx_array_t                     *extra_vars;
    ngx_array_t                     *extra_headers;
    ngx_str_t                        uri;
    ngx_str_t                        args;
    ngx_str_t                        extra_args;
    ngx_uint_t                       flags;
    u_char                          *p;
    u_char                          *q;
    size_t                           len;
    size_t                           nargs;
    int                              rc;
    int                              n;
    ngx_uint_t                       method;
    ngx_http_request_body_t         *body;
    int                              type;
    ngx_buf_t                       *b;
    unsigned                         vars_action;
    ngx_uint_t                       nsubreqs;
    ngx_uint_t                       index;
    size_t                           sr_statuses_len;
    size_t                           sr_headers_len;
    size_t                           sr_bodies_len;
    unsigned                         custom_ctx;

    n = lua_gettop(L);
    if (n != 1) {
        return luaL_error(L, "only one argument is expected, but got %d", n);
    }

    luaL_checktype(L, 1, LUA_TTABLE);

    nsubreqs = lua_objlen(L, 1);
    if (nsubreqs == 0) {
        return luaL_error(L, "at least one subrequest should be specified");
    }

    lua_pushlightuserdata(L, &ngx_http_lua_request_key);
    lua_rawget(L, LUA_GLOBALSINDEX);
    r = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (r == NULL) {
        return luaL_error(L, "no request object found");
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return luaL_error(L, "no ctx found");
    }

    ngx_http_lua_check_context(L, ctx, NGX_HTTP_LUA_CONTEXT_REWRITE
                               | NGX_HTTP_LUA_CONTEXT_ACCESS
                               | NGX_HTTP_LUA_CONTEXT_CONTENT);

    sr_statuses_len = nsubreqs * sizeof(ngx_int_t);
    sr_headers_len  = nsubreqs * sizeof(ngx_http_headers_out_t *);
    sr_bodies_len   = nsubreqs * sizeof(ngx_str_t);

    p = ngx_pcalloc(r->pool, sr_statuses_len + sr_headers_len +
            sr_bodies_len);

    if (p == NULL) {
        return luaL_error(L, "out of memory");
    }

    ctx->sr_statuses = (void *) p;
    p += sr_statuses_len;

    ctx->sr_headers = (void *) p;
    p += sr_headers_len;

    ctx->sr_bodies = (void *) p;

    ctx->nsubreqs = nsubreqs;

    n = lua_gettop(L);
    dd("top before loop: %d", n);

    ctx->done = 0;
    ctx->waiting = 0;

    extra_vars = extra_headers = NULL;

    for (index = 0; index < nsubreqs; index++) {
        ctx->waiting++;

        lua_rawgeti(L, 1, index + 1);
        if (lua_isnil(L, -1)) {
            return luaL_error(L, "only array-like tables are allowed");
        }

        dd("queries query: top %d", lua_gettop(L));

        if (lua_type(L, -1) != LUA_TTABLE) {
            return luaL_error(L, "the query argument %d is not a table, "
                    "but a %s",
                    index, lua_typename(L, lua_type(L, -1)));
        }

        nargs = lua_objlen(L, -1);

        if (nargs != 1 && nargs != 2) {
            return luaL_error(L, "query argument %d expecting one or "
                    "two arguments", index);
        }

        lua_rawgeti(L, 2, 1); /* queries query uri */

        dd("queries query uri: %d", lua_gettop(L));

        dd("first arg in first query: %s", lua_typename(L, lua_type(L, -1)));

        body = NULL;

        extra_args.data = NULL;
        extra_args.len = 0;

        /* flush out existing elements in the arrays */

        if (extra_headers != NULL) {
            extra_headers->nelts = 0;
        }

        if (extra_vars != NULL) {
            extra_vars->nelts = 0;
        }

        vars_action = 0;

        custom_ctx = 0;

        if (nargs == 2) {
            /* check out the options table */

            lua_rawgeti(L, 2, 2); /* queries query uri opts */

            dd("queries query uri opts: %d", lua_gettop(L));

            if (lua_type(L, 4) != LUA_TTABLE) {
                return luaL_error(L, "expecting table as the 2nd argument for "
                        "subrequest %d, but got %s", index,
                        luaL_typename(L, 4));
            }

            dd("queries query uri opts: %d", lua_gettop(L));

            /* check the args option */

            lua_getfield(L, 4, "args");

            type = lua_type(L, -1);

            switch (type) {
            case LUA_TTABLE:
                ngx_http_lua_process_args_option(r, L, -1, &extra_args);
                break;

            case LUA_TNIL:
                /* do nothing */
                break;

            case LUA_TNUMBER:
            case LUA_TSTRING:
                extra_args.data = (u_char *) lua_tolstring(L, -1, &len);
                extra_args.len = len;

                break;

            default:
                return luaL_error(L, "Bad args option value");
            }

            lua_pop(L, 1);

            dd("queries query uri opts: %d", lua_gettop(L));

            /* check the vars option */

            lua_getfield(L, 4, "vars");

            switch (lua_type(L, -1)) {
            case LUA_TTABLE:
                ngx_http_lua_process_vars_option(r, L, -1, &extra_vars);

                dd("post process vars top: %d", lua_gettop(L));
                break;

            case LUA_TNIL:
                /* do nothing */
                break;

            default:
                return luaL_error(L, "Bad vars option value");
            }

            lua_pop(L, 1);

            dd("queries query uri opts: %d", lua_gettop(L));

            /* check the share_all_vars option */

            lua_getfield(L, 4, "share_all_vars");

            switch (lua_type(L, -1)) {
            case LUA_TNIL:
                /* do nothing */
                break;

            case LUA_TBOOLEAN:
                if (lua_toboolean(L, -1)) {
                    vars_action |= NGX_HTTP_LUA_SHARE_ALL_VARS;
                }
                break;

            default:
                return luaL_error(L, "Bad share_all_vars option value");
            }

            lua_pop(L, 1);

            dd("queries query uri opts: %d", lua_gettop(L));

            /* check the copy_all_vars option */

            lua_getfield(L, 4, "copy_all_vars");

            switch (lua_type(L, -1)) {
            case LUA_TNIL:
                /* do nothing */
                break;

            case LUA_TBOOLEAN:
                if (lua_toboolean(L, -1)) {
                    vars_action |= NGX_HTTP_LUA_COPY_ALL_VARS;
                }
                break;

            default:
                return luaL_error(L, "Bad copy_all_vars option value");
            }

            lua_pop(L, 1);

            dd("queries query uri opts: %d", lua_gettop(L));

            /* check the "method" option */

            lua_getfield(L, 4, "method");

            type = lua_type(L, -1);

            if (type == LUA_TNIL) {
                method = NGX_HTTP_GET;

            } else {
                if (type != LUA_TNUMBER) {
                    return luaL_error(L, "Bad http request method");
                }

                method = (ngx_uint_t) lua_tonumber(L, -1);
            }

            lua_pop(L, 1);

            dd("queries query uri opts: %d", lua_gettop(L));

            /* check the "headers" option */

            lua_getfield(L, 4, "extra_headers");

            type = lua_type(L, -1);

            if (type != LUA_TNIL) {
                if (type != LUA_TTABLE) {
                    return luaL_error(L, "Bad extra_headers option value type %s, "
                            "expected a Lua table", lua_typename(L, type));
                }
                ngx_http_lua_process_extra_headers_option(r, L, &extra_headers);
            }

            lua_pop(L, 1);

            dd("queries query uri opts: %d", lua_gettop(L));

            /* check the "ctx" option */

            lua_getfield(L, 4, "ctx");

            type = lua_type(L, -1);

            if (type != LUA_TNIL) {
                if (type != LUA_TTABLE) {
                    return luaL_error(L, "Bad ctx option value type %s, "
                            "expected a Lua table", lua_typename(L, type));
                }
                custom_ctx = 1;
            } else {
                lua_pop(L, 1);
            }

            dd("queries query uri opts ctx?: %d", lua_gettop(L));

            /* check the "body" option */

            lua_getfield(L, 4, "body");

            type = lua_type(L, -1);

            if (type != LUA_TNIL) {
                if (type != LUA_TSTRING && type != LUA_TNUMBER) {
                    return luaL_error(L, "Bad http request body");
                }

                body = ngx_pcalloc(r->pool,
                        sizeof(ngx_http_request_body_t));

                if (body == NULL) {
                    return luaL_error(L, "out of memory");
                }

                q = (u_char *) lua_tolstring(L, -1, &len);

                dd("request body: [%.*s]", (int) len, q);

                if (len) {
                    b = ngx_create_temp_buf(r->pool, len);
                    if (b == NULL) {
                        return luaL_error(L, "out of memory");
                    }

                    b->last = ngx_copy(b->last, q, len);

                    body->bufs = ngx_alloc_chain_link(r->pool);
                    if (body->bufs == NULL) {
                        return luaL_error(L, "out of memory");
                    }

                    body->bufs->buf = b;
                    body->bufs->next = NULL;

                    body->buf = b;
                }
            }

            lua_pop(L, 1); /* pop the body */

            /* stack: queries query uri opts ctx? */

            lua_remove(L, 4);

            /* stack: queries query uri ctx? */

            dd("queries query uri ctx?: %d", lua_gettop(L));

        } else {
            method = NGX_HTTP_GET;
        }

        /* stack: queries query uri ctx? */

        n = lua_gettop(L);
        dd("top size so far: %d", n);

        p = (u_char *) luaL_checklstring(L, 3, &len);

        uri.data = ngx_palloc(r->pool, len);
        if (uri.data == NULL) {
            return luaL_error(L, "memory allocation error");
        }

        ngx_memcpy(uri.data, p, len);

        uri.len = len;

        args.data = NULL;
        args.len = 0;

        flags = 0;

        rc = ngx_http_parse_unsafe_uri(r, &uri, &args, &flags);
        if (rc != NGX_OK) {
            dd("rc = %d", (int) rc);

            return luaL_error(L, "unsafe uri in argument #1: %s", p);
        }

        if (args.len == 0) {
            args = extra_args;

        } else if (extra_args.len) {
            /* concatenate the two parts of args together */
            len = args.len + (sizeof("&") - 1) + extra_args.len;

            p = ngx_palloc(r->pool, len);
            if (p == NULL) {
                return luaL_error(L, "out of memory");
            }

            q = ngx_copy(p, args.data, args.len);
            *q++ = '&';
            ngx_memcpy(q, extra_args.data, extra_args.len);

            args.data = p;
            args.len = len;
        }

        psr = ngx_palloc(r->pool, sizeof(ngx_http_post_subrequest_t));
        if (psr == NULL) {
            return luaL_error(L, "memory allocation error");
        }

        sr_ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_lua_ctx_t));
        if (sr_ctx == NULL) {
            return luaL_error(L, "out of memory");
        }

        /* set by ngx_pcalloc:
         *      sr_ctx->run_post_subrequest = 0
         *      sr_ctx->free = NULL
         */

        sr_ctx->cc_ref = LUA_NOREF;
        sr_ctx->ctx_ref = LUA_NOREF;

        sr_ctx->capture = 1;

        sr_ctx->index = index;

        psr->handler = ngx_http_lua_post_subrequest;
        psr->data = sr_ctx;

        rc = ngx_http_subrequest(r, &uri, &args, &sr, psr, 0);

        if (rc != NGX_OK) {
            return luaL_error(L, "failed to issue subrequest: %d", (int) rc);
        }

        ngx_http_set_ctx(sr, sr_ctx, ngx_http_lua_module);

        rc = ngx_http_lua_adjust_subrequest(sr, method, extra_headers, body,
                vars_action, extra_vars);

        if (rc != NGX_OK) {
            return luaL_error(L, "failed to adjust the subrequest: %d",
                    (int) rc);
        }

        dd("queries query uri opts ctx? %d", lua_gettop(L));

        /* stack: queries query uri ctx? */

        if (custom_ctx) {
            ngx_http_lua_ngx_set_ctx_helper(L, sr, sr_ctx, -1);
            lua_pop(L, 3);

        } else {
            lua_pop(L, 2);
        }

        /* stack: queries */
    }

    if (extra_headers) {
        ngx_array_destroy(extra_headers);
    }

    if (extra_vars) {
        ngx_array_destroy(extra_vars);
    }

    return lua_yield(L, 0);
}


static ngx_int_t
ngx_http_lua_adjust_subrequest(ngx_http_request_t *sr, ngx_uint_t method,
    ngx_array_t *extra_headers, ngx_http_request_body_t *body,
    unsigned vars_action, ngx_array_t *extra_vars)
{
    ngx_http_request_t          *r;
    ngx_int_t                    rc;
    ngx_http_core_main_conf_t   *cmcf;
    size_t                       size;

    r = sr->parent;

    sr->header_in = r->header_in;

    if (body) {
        sr->request_body = body;

        rc = ngx_http_lua_sub_request_set_headers(sr,
                body->buf ? ngx_buf_size(body->buf) : 0, extra_headers);

        if (rc != NGX_OK) {
            return NGX_ERROR;
        }

    } else if (method != NGX_HTTP_PUT && method != NGX_HTTP_POST
               && r->headers_in.content_length_n > 0)
    {
        rc = ngx_http_lua_sub_request_set_headers(sr, 0, extra_headers);
        if (rc != NGX_OK) {
            return NGX_ERROR;
        }

#if 1
        sr->request_body = NULL;
#endif
    }

    sr->method = method;

    switch (method) {
        case NGX_HTTP_GET:
            sr->method_name = ngx_http_lua_get_method;
            break;

        case NGX_HTTP_POST:
            sr->method_name = ngx_http_lua_post_method;
            break;

        case NGX_HTTP_PUT:
            sr->method_name = ngx_http_lua_put_method;
            break;

        case NGX_HTTP_HEAD:
            sr->method_name = ngx_http_lua_head_method;
            break;

        case NGX_HTTP_DELETE:
            sr->method_name = ngx_http_lua_delete_method;
            break;

        case NGX_HTTP_OPTIONS:
            sr->method_name = ngx_http_lua_options_method;
            break;

        default:
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "unsupported HTTP method: %u", (unsigned) method);

            return NGX_ERROR;
    }

    /* XXX work-around a bug in ngx_http_subrequest */
    if (r->headers_in.headers.last == &r->headers_in.headers.part) {
        sr->headers_in.headers.last = &sr->headers_in.headers.part;
    }

    if (!(vars_action & NGX_HTTP_LUA_SHARE_ALL_VARS)) {
        /* we do not inherit the parent request's variables */
        cmcf = ngx_http_get_module_main_conf(sr, ngx_http_core_module);

        size = cmcf->variables.nelts * sizeof(ngx_http_variable_value_t);

        if (vars_action & NGX_HTTP_LUA_COPY_ALL_VARS) {

            sr->variables = ngx_palloc(sr->pool, size);
            if (sr->variables == NULL) {
                return NGX_ERROR;
            }

            ngx_memcpy(sr->variables, r->variables, size);

        } else {

            /* we do not inherit the parent request's variables */

            sr->variables = ngx_pcalloc(sr->pool, size);
            if (sr->variables == NULL) {
                return NGX_ERROR;
            }
        }
    }

    return ngx_http_lua_subrequest_add_extra_vars(sr, extra_vars);
}


static ngx_int_t
ngx_http_lua_subrequest_add_extra_vars(ngx_http_request_t *sr,
       ngx_array_t *extra_vars)
{
    ngx_http_core_main_conf_t   *cmcf;
    ngx_http_variable_t         *v;
    ngx_http_variable_value_t   *vv;
    u_char                      *val;
    u_char                      *p;
    ngx_uint_t                   i, hash;
    ngx_str_t                    name;
    size_t                       len;
    ngx_hash_t                  *variables_hash;
    ngx_keyval_t                *var;

    /* set any extra variables that were passed to the subrequest */

    if (extra_vars == NULL || extra_vars->nelts == 0) {
        return NGX_OK;
    }

    cmcf = ngx_http_get_module_main_conf(sr, ngx_http_core_module);

    variables_hash = &cmcf->variables_hash;

    var = extra_vars->elts;

    for (i = 0; i < extra_vars->nelts; i++, var++) {
        /* copy the variable's name and value because they are allocated
         * by the lua VM */

        len = var->key.len + var->value.len;

        p = ngx_pnalloc(sr->pool, len);
        if (p == NULL) {
            return NGX_ERROR;
        }

        name.data = p;
        name.len = var->key.len;

        p = ngx_copy(p, var->key.data, var->key.len);

        hash = ngx_hash_strlow(name.data, name.data, name.len);

        val = p;
        len = var->value.len;

        ngx_memcpy(p, var->value.data, len);

        v = ngx_hash_find(variables_hash, hash, name.data, name.len);

        if (v) {
            if (!(v->flags & NGX_HTTP_VAR_CHANGEABLE)) {
                ngx_log_error(NGX_LOG_ERR, sr->connection->log, 0,
                              "variable \"%V\" not changeable", &name);
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            if (v->set_handler) {
                vv = ngx_palloc(sr->pool, sizeof(ngx_http_variable_value_t));
                if (vv == NULL) {
                    return NGX_ERROR;
                }

                vv->valid = 1;
                vv->not_found = 0;
                vv->no_cacheable = 0;

                vv->data = val;
                vv->len = len;

                v->set_handler(sr, vv, v->data);

                ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sr->connection->log, 0,
                               "variable \"%V\" set to value \"%v\"", &name,
                               vv);

                continue;
            }

            if (v->flags & NGX_HTTP_VAR_INDEXED) {
                vv = &sr->variables[v->index];

                vv->valid = 1;
                vv->not_found = 0;
                vv->no_cacheable = 0;

                vv->data = val;
                vv->len = len;

                ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sr->connection->log, 0,
                      "variable \"%V\" set to value \"%v\"", &name, vv);

                continue;
            }
        }

        ngx_log_error(NGX_LOG_ERR, sr->connection->log, 0,
                      "variable \"%V\" cannot be assigned a value (maybe you "
                      "forgot to define it first?) ", &name);

        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_http_lua_process_extra_headers_option(ngx_http_request_t *r, lua_State *L,
        ngx_array_t **headersp)
{

    ngx_array_t         *headers;
    ngx_keyval_t        *header;
    int                 table = lua_gettop(L);

    headers = *headersp;

    if (headers == NULL) {

        headers = ngx_array_create(r->pool, 4, sizeof(ngx_keyval_t));
        if (headers == NULL) {
            luaL_error(L, "out of memory");
            return;
        }

        *headersp = headers;
    }

    lua_pushnil(L);

    while (lua_next(L, table) != 0) {

        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "attempt to use a non-string key in the "
                    "\"headers\" option table");
            return;
        }

        if (!lua_isstring(L, -1)) {
            luaL_error(L, "attempt to use bad variable value type %s",
                       luaL_typename(L, -1));
        }

        header = ngx_array_push(headers);
        if (header == NULL) {
            luaL_error(L, "out of memory");
            return;
        }

        header->key.data = (u_char *) lua_tolstring(L, -2, &header->key.len);
        header->value.data = (u_char *) lua_tolstring(L, -1, &header->value.len);

        lua_pop(L, 1);

    }

}


static void
ngx_http_lua_process_vars_option(ngx_http_request_t *r, lua_State *L,
        int table, ngx_array_t **varsp)
{
    ngx_array_t         *vars;
    ngx_keyval_t        *var;

    if (table < 0) {
        table = lua_gettop(L) + table + 1;
    }

    vars = *varsp;

    if (vars == NULL) {

        vars = ngx_array_create(r->pool, 4, sizeof(ngx_keyval_t));
        if (vars == NULL) {
            dd("here");
            luaL_error(L, "out of memory");
            return;
        }

        *varsp = vars;
    }

    lua_pushnil(L);
    while (lua_next(L, table) != 0) {

        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "attempt to use a non-string key in the "
                    "\"vars\" option table");
            return;
        }

        if (!lua_isstring(L, -1)) {
            luaL_error(L, "attempt to use bad variable value type %s",
                       luaL_typename(L, -1));
        }

        var = ngx_array_push(vars);
        if (var == NULL) {
            dd("here");
            luaL_error(L, "out of memory");
            return;
        }

        var->key.data = (u_char *) lua_tolstring(L, -2, &var->key.len);
        var->value.data = (u_char *) lua_tolstring(L, -1, &var->value.len);

        lua_pop(L, 1);
    }
}


ngx_int_t
ngx_http_lua_post_subrequest(ngx_http_request_t *r, void *data, ngx_int_t rc)
{
    ngx_http_request_t            *pr;
    ngx_http_lua_ctx_t            *pr_ctx;
    ngx_http_lua_ctx_t            *ctx = data;
    size_t                         len;
    ngx_str_t                     *body_str;
    u_char                        *p;
    ngx_chain_t                   *cl;

    if (ctx->run_post_subrequest) {
        return rc;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "lua run post subrequest handler: rc:%d, waiting:%d, done:%d",
            rc, ctx->waiting, ctx->done);

    ctx->run_post_subrequest = 1;

#if 0
    ngx_http_lua_dump_postponed(r);
#endif

    pr = r->parent;

    pr_ctx = ngx_http_get_module_ctx(pr, ngx_http_lua_module);
    if (pr_ctx == NULL) {
        return NGX_ERROR;
    }

    pr_ctx->waiting--;

    if (pr_ctx->waiting == 0) {
        pr_ctx->done = 1;
    }

    if (pr_ctx->entered_content_phase) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "lua restoring write event handler");

        pr->write_event_handler = ngx_http_lua_content_wev_handler;
    }

    dd("status rc = %d", (int) rc);
    dd("status headers_out.status = %d", (int) r->headers_out.status);
    dd("uri: %.*s", (int) r->uri.len, r->uri.data);

    /*  capture subrequest response status */
    if (rc == NGX_ERROR) {
        pr_ctx->sr_statuses[ctx->index] = NGX_HTTP_INTERNAL_SERVER_ERROR;

    } else if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        dd("HERE");
        pr_ctx->sr_statuses[ctx->index] = rc;

    } else {
        dd("THERE");
        pr_ctx->sr_statuses[ctx->index] = r->headers_out.status;
    }

    if (pr_ctx->sr_statuses[ctx->index] == 0) {
        pr_ctx->sr_statuses[ctx->index] = NGX_HTTP_OK;
    }

    dd("pr_ctx status: %d", (int) pr_ctx->sr_statuses[ctx->index]);

    /* copy subrequest response headers */

    pr_ctx->sr_headers[ctx->index] = &r->headers_out;

    /* copy subrequest response body */

    body_str = &pr_ctx->sr_bodies[ctx->index];

    len = 0;
    for (cl = ctx->body; cl; cl = cl->next) {
        /*  ignore all non-memory buffers */
        len += cl->buf->last - cl->buf->pos;
    }

    body_str->len = len;

    if (len == 0) {
        body_str->data = NULL;

    } else {
        p = ngx_palloc(r->pool, len);
        if (p == NULL) {
            return NGX_ERROR;
        }

        body_str->data = p;

        /* copy from and then free the data buffers */

        for (cl = ctx->body; cl; cl = cl->next) {
            p = ngx_copy(p, cl->buf->pos,
                    cl->buf->last - cl->buf->pos);

            cl->buf->last = cl->buf->pos;

#if 0
            dd("free body chain link buf ASAP");
            ngx_pfree(r->pool, cl->buf->start);
#endif
        }
    }

    if (ctx->body) {

#if defined(nginx_version) && nginx_version >= 1001004
        ngx_chain_update_chains(r->pool,
#else
        ngx_chain_update_chains(
#endif
                                &pr_ctx->free_bufs, &pr_ctx->busy_bufs,
                                &ctx->body,
                                (ngx_buf_tag_t) &ngx_http_lua_module);

        dd("free bufs: %p", pr_ctx->free_bufs);
    }

    /* work-around issues in nginx's event module */

    if (r != r->connection->data && r->postponed &&
            (r->main->posted_requests == NULL ||
            r->main->posted_requests->request != pr))
    {
#if defined(nginx_version) && nginx_version >= 8012
        ngx_http_post_request(pr, NULL);
#else
        ngx_http_post_request(pr);
#endif
    }

    return rc;
}


static ngx_int_t
ngx_http_lua_sub_request_set_extra_headers(ngx_http_request_t *r,
        ngx_array_t *extra_headers)
{

    ngx_table_elt_t                 *h;
    ngx_keyval_t                    *var;
    ngx_uint_t                       header_index;

    var = extra_headers->elts;

    for (header_index = 0; header_index < extra_headers->nelts;
         header_index++, var++) {

        if (ngx_http_lua_content_length_header_key.len == var->key.len &&
            ngx_strncasecmp(ngx_http_lua_content_length_header_key.data,
                            var->key.data,
                            ngx_http_lua_content_length_header_key.len) == 0)
        {
            continue;
        }

        h = ngx_list_push(&r->headers_in.headers);

        if (h == NULL) {
            return NGX_ERROR;
        }

        // copy header's name

        h->key.data = ngx_pnalloc(r->pool, var->key.len);

        if (h->key.data == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(h->key.data, var->key.data, var->key.len);
        h->key.len = var->key.len;

        // copy header's value

        h->value.data = ngx_pnalloc(r->pool, var->value.len);

        if (h->value.data == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(h->value.data, var->value.data, var->value.len);
        h->value.len = var->value.len;

        // set lowcase_key value

        h->lowcase_key = ngx_pnalloc(r->pool, h->key.len);
        if (h->lowcase_key == NULL) {
            return NGX_ERROR;
        }

        ngx_strlow(h->lowcase_key, h->key.data, h->key.len);

        h->hash = ngx_hash_key_lc(h->key.data, h->key.len);

    }

    return NGX_OK;

}


static ngx_int_t
ngx_http_lua_sub_request_set_content_length_header(ngx_http_request_t *r,
        off_t len)
{

    ngx_table_elt_t                 *h;
    u_char                          *p;

    r->headers_in.content_length_n = len;

    h = ngx_list_push(&r->headers_in.headers);

    if (h == NULL) {
        return NGX_ERROR;
    }

    h->key = ngx_http_lua_content_length_header_key;
    h->lowcase_key = ngx_pnalloc(r->pool, h->key.len);
    if (h->lowcase_key == NULL) {
        return NGX_ERROR;
    }

    ngx_strlow(h->lowcase_key, h->key.data, h->key.len);

    r->headers_in.content_length = h;

    p = ngx_palloc(r->pool, NGX_OFF_T_LEN);
    if (p == NULL) {
        return NGX_ERROR;
    }

    h->value.data = p;

    h->value.len = ngx_sprintf(h->value.data, "%O", len) - h->value.data;

    h->hash = ngx_hash(ngx_hash(ngx_hash(ngx_hash(ngx_hash(ngx_hash(ngx_hash(
            ngx_hash(ngx_hash(ngx_hash(ngx_hash(ngx_hash(
            ngx_hash('c', 'o'), 'n'), 't'), 'e'), 'n'), 't'), '-'), 'l'), 'e'),
            'n'), 'g'), 't'), 'h');

#if 0
    dd("content length hash: %lu == %lu", (unsigned long) h->hash,
            ngx_hash_key_lc((u_char *) "Content-Length",
            sizeof("Content-Length") - 1));
#endif

    dd("r content length: %.*s",
            (int)r->headers_in.content_length->value.len,
            r->headers_in.content_length->value.data);

    return NGX_OK;

}


static ngx_int_t
ngx_http_lua_sub_request_copy_parent_headers(ngx_http_request_t *r)
{

    ngx_table_elt_t                 *h, *header, *pr_header;
    ngx_list_part_t                 *headers_part, *pr_headers_part;
    ngx_http_request_t              *pr;
    ngx_uint_t                       header_index, pr_header_index,
                                     pr_header_len;

    pr = r->parent;

    if (pr == NULL) {
        return NGX_OK;
    }

    /* forward the parent request's all other request headers */

    pr_headers_part = &pr->headers_in.headers.part;
    pr_header = pr_headers_part->elts;

    for (pr_header_index = 0; /* void */; pr_header_index++) {

        if (pr_header_index >= pr_headers_part->nelts) {
            if (pr_headers_part->next == NULL) {
                break;
            }
            pr_headers_part = pr_headers_part->next;
            pr_header = pr_headers_part->elts;
            pr_header_index = 0;
        }

        pr_header_len = pr_header[pr_header_index].key.len;

        headers_part = &r->headers_in.headers.part;
        header = headers_part->elts;

        for (header_index = 0; /* void */; header_index++) {

            if (header_index >= headers_part->nelts) {
                if (headers_part->next == NULL) {
                    break;
                }
                headers_part = headers_part->next;
                header = headers_part->elts;
                header_index = 0;
            }

            if (pr_header_len == header[header_index].key.len &&
                ngx_strncasecmp(pr_header[pr_header_index].key.data,
                                header[header_index].key.data,
                                pr_header_len) == 0)
            {
                break;
            }

        }

        if (headers_part->next != NULL) {
            continue;
        }

        h = ngx_list_push(&r->headers_in.headers);
        if (h == NULL) {
            return NGX_ERROR;
        }

        *h = pr_header[pr_header_index];
    }

    return NGX_OK;

}


static ngx_int_t
ngx_http_lua_sub_request_set_headers(ngx_http_request_t *r, off_t content_len,
        ngx_array_t *extra_headers)
{

    ngx_uint_t                       rc;

    if (ngx_list_init(&r->headers_in.headers, r->pool, 20,
                sizeof(ngx_table_elt_t)) != NGX_OK) {
        return NGX_ERROR;
    }

    rc = ngx_http_lua_sub_request_set_content_length_header(r, content_len);

    if (rc != NGX_OK) {
        return rc;
    }


    if (extra_headers) {

        rc = ngx_http_lua_sub_request_set_extra_headers(r, extra_headers);

        if (rc != NGX_OK) {
            return rc;
        }

    }

    rc = ngx_http_lua_sub_request_copy_parent_headers(r);

    if (rc != NGX_OK) {
        return rc;
    }

    /* XXX maybe we should set those built-in header slot in
     * ngx_http_headers_in_t too? */

    return NGX_OK;

}


void
ngx_http_lua_handle_subreq_responses(ngx_http_request_t *r,
        ngx_http_lua_ctx_t *ctx)
{
    ngx_uint_t                   index;
    lua_State                   *cc;
    ngx_str_t                   *body_str;
    ngx_table_elt_t             *header;
    ngx_list_part_t             *part;
    ngx_http_headers_out_t      *sr_headers;
    ngx_uint_t                   i;

    u_char                  buf[sizeof("Mon, 28 Sep 1970 06:00:00 GMT") - 1];

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "lua handle subrequest responses");

    for (index = 0; index < ctx->nsubreqs; index++) {
        dd("summary: reqs %d, subquery %d, waiting %d, req %.*s",
                (int) ctx->nsubreqs,
                (int) index,
                (int) ctx->waiting,
                (int) r->uri.len, r->uri.data);

        cc = ctx->cc;

        /*  {{{ construct ret value */
        lua_newtable(cc);

        /*  copy captured status */
        lua_pushinteger(cc, ctx->sr_statuses[index]);
        lua_setfield(cc, -2, "status");

        /*  copy captured body */

        body_str = &ctx->sr_bodies[index];

        lua_pushlstring(cc, (char *) body_str->data, body_str->len);
        lua_setfield(cc, -2, "body");

        if (body_str->data) {
            dd("free body buffer ASAP");
            ngx_pfree(r->pool, body_str->data);
        }

        /* copy captured headers */

        lua_newtable(cc); /* res.header */

        sr_headers = ctx->sr_headers[index];

        dd("saving subrequest response headers");

        part = &sr_headers->headers.part;
        header = part->elts;

        for (i = 0; /* void */; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }

                part = part->next;
                header = part->elts;
                i = 0;
            }

            dd("checking sr header %.*s", (int) header[i].key.len,
                    header[i].key.data);

#if 1
            if (header[i].hash == 0) {
                continue;
            }
#endif

            header[i].hash = 0;

            dd("pushing sr header %.*s", (int) header[i].key.len,
                    header[i].key.data);

            lua_pushlstring(cc, (char *) header[i].key.data,
                    header[i].key.len); /* header key */
            lua_pushvalue(cc, -1); /* stack: table key key */

            /* check if header already exists */
            lua_rawget(cc, -3); /* stack: table key value */

            if (lua_isnil(cc, -1)) {
                lua_pop(cc, 1); /* stack: table key */

                lua_pushlstring(cc, (char *) header[i].value.data,
                        header[i].value.len); /* stack: table key value */

                lua_rawset(cc, -3); /* stack: table */

            } else {

                if (!lua_istable(cc, -1)) { /* already inserted one value */
                    lua_createtable(cc, 4, 0);
                        /* stack: table key value table */

                    lua_insert(cc, -2); /* stack: table key table value */
                    lua_rawseti(cc, -2, 1); /* stack: table key table */

                    lua_pushlstring(cc, (char *) header[i].value.data,
                            header[i].value.len);
                        /* stack: table key table value */

                    lua_rawseti(cc, -2, lua_objlen(cc, -2) + 1);
                        /* stack: table key table */

                    lua_rawset(cc, -3); /* stack: table */

                } else {
                    lua_pushlstring(cc, (char *) header[i].value.data,
                            header[i].value.len);
                        /* stack: table key table value */

                    lua_rawseti(cc, -2, lua_objlen(cc, -2) + 1);
                        /* stack: table key table */

                    lua_pop(cc, 2); /* stack: table */
                }
            }
        }

        if (sr_headers->content_type.len) {
            lua_pushliteral(cc, "Content-Type"); /* header key */
            lua_pushlstring(cc, (char *) sr_headers->content_type.data,
                    sr_headers->content_type.len); /* head key value */
            lua_rawset(cc, -3); /* head */
        }

        if (sr_headers->content_length == NULL
            && sr_headers->content_length_n >= 0)
        {
            lua_pushliteral(cc, "Content-Length"); /* header key */

            lua_pushnumber(cc, sr_headers->content_length_n);
                /* head key value */

            lua_rawset(cc, -3); /* head */
        }

        /* to work-around an issue in ngx_http_static_module
         * (github issue #41) */
        if (sr_headers->location && sr_headers->location->value.len) {
            lua_pushliteral(cc, "Location"); /* header key */
            lua_pushlstring(cc, (char *) sr_headers->location->value.data,
                    sr_headers->location->value.len); /* head key value */
            lua_rawset(cc, -3); /* head */
        }

        if (sr_headers->last_modified_time != -1) {
            if (sr_headers->status != NGX_HTTP_OK
                && sr_headers->status != NGX_HTTP_PARTIAL_CONTENT
                && sr_headers->status != NGX_HTTP_NOT_MODIFIED
                && sr_headers->status != NGX_HTTP_NO_CONTENT)
            {
                sr_headers->last_modified_time = -1;
                sr_headers->last_modified = NULL;
            }
        }

        if (sr_headers->last_modified == NULL
            && sr_headers->last_modified_time != -1)
        {
            (void) ngx_http_time(buf, sr_headers->last_modified_time);

            lua_pushliteral(cc, "Last-Modified"); /* header key */
            lua_pushlstring(cc, (char *) buf, sizeof(buf)); /* head key value */
            lua_rawset(cc, -3); /* head */
        }

        lua_setfield(cc, -2, "header");

        /*  }}} */
    }
}


void
ngx_http_lua_inject_subrequest_api(lua_State *L)
{
    lua_createtable(L, 0 /* narr */, 2 /* nrec */); /* .location */

    lua_pushcfunction(L, ngx_http_lua_ngx_location_capture);
    lua_setfield(L, -2, "capture");

    lua_pushcfunction(L, ngx_http_lua_ngx_location_capture_multi);
    lua_setfield(L, -2, "capture_multi");

    lua_setfield(L, -2, "location");
}

