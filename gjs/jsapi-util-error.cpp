/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright (c) 2008  litl, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <config.h>

#include "jsapi-util.h"
#include "jsapi-wrapper.h"
#include "gi/gerror.h"
#include "util/misc.h"

#include <util/log.h>

#include <string.h>

/*
 * See:
 * https://bugzilla.mozilla.org/show_bug.cgi?id=166436
 * https://bugzilla.mozilla.org/show_bug.cgi?id=215173
 *
 * Very surprisingly, jsapi.h lacks any way to "throw new Error()"
 *
 * So here is an awful hack inspired by
 * http://egachine.berlios.de/embedding-sm-best-practice/embedding-sm-best-practice.html#error-handling
 */
static void
G_GNUC_PRINTF(4, 0)
gjs_throw_valist(JSContext       *context,
                 JSProtoKey       error_kind,
                 const char      *error_name,
                 const char      *format,
                 va_list          args)
{
    char *s;
    bool result;

    s = g_strdup_vprintf(format, args);

    JSAutoCompartment compartment(context, gjs_get_import_global(context));

    JS_BeginRequest(context);

    if (JS_IsExceptionPending(context)) {
        /* Often it's unclear whether a given jsapi.h function
         * will throw an exception, so we will throw ourselves
         * "just in case"; in those cases, we don't want to
         * overwrite an exception that already exists.
         * (Do log in case our second exception adds more info,
         * but don't log as topic ERROR because if the exception is
         * caught we don't want an ERROR in the logs.)
         */
        gjs_debug(GJS_DEBUG_CONTEXT,
                  "Ignoring second exception: '%s'",
                  s);
        g_free(s);
        JS_EndRequest(context);
        return;
    }

    JS::RootedObject constructor(context);
    JS::RootedObject global(context, JS::CurrentGlobalOrNull(context));
    JS::RootedValue v_constructor(context), exc_val(context);
    JS::RootedObject new_exc(context);
    JS::AutoValueArray<1> error_args(context);
    result = false;

    if (!gjs_string_from_utf8(context, s, -1, error_args[0])) {
        JS_ReportErrorUTF8(context, "Failed to copy exception string");
        goto out;
    }

    if (!JS_GetClassObject(context, error_kind, &constructor))
        goto out;

    /* throw new Error(message) */
    new_exc = JS_New(context, constructor, error_args);

    if (!new_exc)
        goto out;

    if (error_name != NULL) {
        JS::RootedValue name_value(context);
        if (!gjs_string_from_utf8(context, error_name, -1, &name_value) ||
            !gjs_object_set_property(context, new_exc, GJS_STRING_NAME,
                                     name_value))
            goto out;
    }

    exc_val.setObject(*new_exc);
    JS_SetPendingException(context, exc_val);

    result = true;

 out:

    if (!result) {
        /* try just reporting it to error handler? should not
         * happen though pretty much
         */
        JS_ReportErrorUTF8(context, "Failed to throw exception '%s'", s);
    }
    g_free(s);

    JS_EndRequest(context);
}

/* Throws an exception, like "throw new Error(message)"
 *
 * If an exception is already set in the context, this will
 * NOT overwrite it. That's an important semantic since
 * we want the "root cause" exception. To overwrite,
 * use JS_ClearPendingException() first.
 */
void
gjs_throw(JSContext       *context,
          const char      *format,
          ...)
{
    va_list args;

    va_start(args, format);
    gjs_throw_valist(context, JSProto_Error, nullptr, format, args);
    va_end(args);
}

/*
 * Like gjs_throw, but allows to customize the error
 * class and 'name' property. Mainly used for throwing TypeError instead of
 * error.
 */
void
gjs_throw_custom(JSContext  *cx,
                 JSProtoKey  kind,
                 const char *error_name,
                 const char *format,
                 ...)
{
    va_list args;
    g_return_if_fail(kind == JSProto_Error || kind == JSProto_InternalError ||
        kind == JSProto_EvalError || kind == JSProto_RangeError ||
        kind == JSProto_ReferenceError || kind == JSProto_SyntaxError ||
        kind == JSProto_TypeError || kind == JSProto_URIError ||
        kind == JSProto_StopIteration);

    va_start(args, format);
    gjs_throw_valist(cx, kind, error_name, format, args);
    va_end(args);
}

/**
 * gjs_throw_literal:
 *
 * Similar to gjs_throw(), but does not treat its argument as
 * a format string.
 */
void
gjs_throw_literal(JSContext       *context,
                  const char      *string)
{
    gjs_throw(context, "%s", string);
}

/**
 * gjs_throw_g_error:
 *
 * Convert a GError into a JavaScript Exception, and
 * frees the GError. Differently from gjs_throw(), it
 * will overwrite an existing exception, as it is used
 * to report errors from C functions.
 */
void
gjs_throw_g_error (JSContext       *context,
                   GError          *error)
{
    if (error == NULL)
        return;

    JS_BeginRequest(context);

    JS::RootedValue err(context,
        JS::ObjectOrNullValue(gjs_error_from_gerror(context, error, true)));
    g_error_free (error);
    if (!err.isNull())
        JS_SetPendingException(context, err);

    JS_EndRequest(context);
}

/**
 * gjs_format_stack_trace:
 * @cx: the #JSContext
 * @saved_frame: a SavedFrame #JSObject
 *
 * Formats a stack trace as a string in filename encoding, suitable for
 * printing to stderr. Ignores any errors.
 *
 * Returns: unique string in filename encoding, or nullptr if no stack trace
 */
GjsAutoChar
gjs_format_stack_trace(JSContext       *cx,
                       JS::HandleObject saved_frame)
{
    JS::AutoSaveExceptionState saved_exc(cx);

    JS::RootedString stack_trace(cx);
    GjsAutoJSChar stack_utf8(cx);
    if (JS::BuildStackString(cx, saved_frame, &stack_trace, 2))
        stack_utf8.reset(cx, JS_EncodeStringToUTF8(cx, stack_trace));

    saved_exc.restore();

    if (!stack_utf8)
        return nullptr;

    return g_filename_from_utf8(stack_utf8, -1, nullptr, nullptr, nullptr);
}

void
gjs_warning_reporter(JSContext     *context,
                     JSErrorReport *report)
{
    const char *warning;
    GLogLevelFlags level;

    g_assert(report);

    if (gjs_environment_variable_is_set("GJS_ABORT_ON_OOM") &&
        report->flags == JSREPORT_ERROR &&
        report->errorNumber == 137) {
        /* 137, JSMSG_OUT_OF_MEMORY */
        g_error("GJS ran out of memory at %s: %i.",
                report->filename,
                report->lineno);
    }

    if ((report->flags & JSREPORT_WARNING) != 0) {
        warning = "WARNING";
        level = G_LOG_LEVEL_MESSAGE;

        /* suppress bogus warnings. See mozilla/js/src/js.msg */
        if (report->errorNumber == 162) {
            /* 162, JSMSG_UNDEFINED_PROP: warns every time a lazy property
             * is resolved, since the property starts out
             * undefined. When this is a real bug it should usually
             * fail somewhere else anyhow.
             */
            return;
        }
    } else {
        warning = "REPORTED";
        level = G_LOG_LEVEL_WARNING;
    }

    g_log(G_LOG_DOMAIN, level, "JS %s: [%s %d]: %s", warning, report->filename,
          report->lineno, report->message().c_str());
}
