#include "Common.hpp"
#include "GetOpt.hpp"
#include "util/StringUtil.hpp"
#include "ZCMGen.hpp"
#include "Emitter.hpp"

#define FLAG_NONE 0

// flags for emit_c_array_loops_start
#define FLAG_EMIT_MALLOCS 1

// flags for emit_c_array_loops_end
#define FLAG_EMIT_FREES   2

static string dotsToUnderscores(const string& s)
{
    string ret = s;
    for (uint i = 0; i < ret.size(); i++)
        if (ret[i] == '.')
             ret[i] = '_';
    return ret;
}

// Create an accessor for member lm, whose name is "n". For arrays,
// the dim'th dimension is accessed. E.g., dim=0 will have no
// additional brackets, dim=1 has [a], dim=2 has [a][b].
static string makeAccessor(ZCMMember& lm, const string& n, uint dim)
{
    if (lm.dimensions.size() == 0) {
        return "&("+n+"[element]."+lm.membername+")";
    } else {
        string s = n+"[element]."+lm.membername;
        for (uint d = 0; d < dim; d++)
            s += "[" + string(1, d+'a') + "]";
        return s;
    }
}

static string makeArraySize(ZCMMember& lm, const string& n, int dim)
{
    if (lm.dimensions.size() == 0) {
        return "1";
    } else {
        auto& ld = lm.dimensions[dim];
        switch (ld.mode) {
            case ZCM_CONST: return ld.size;
            case ZCM_VAR:   return n+"[element]."+ld.size;
        }
    }
    assert(0 && "Should be unreachable");
}

// Some types do not have a 1:1 mapping from zcm types to native C storage types.
static string mapTypeName(const string& t)
{
    if (t == "boolean") return "int8_t";
    if (t == "string")  return "char*";
    if (t == "byte")    return "uint8_t";

    return dotsToUnderscores(t);
}

struct Emit : public Emitter
{
    ZCMGen& zcm;
    ZCMStruct& lr;

    Emit(ZCMGen& zcm, ZCMStruct& lr, const string& fname):
        Emitter(fname), zcm(zcm), lr(lr) {}

    void emitAutoGeneratedWarning()
    {
        emit(0, "// THIS IS AN AUTOMATICALLY GENERATED FILE.  DO NOT MODIFY");
        emit(0, "// BY HAND!!");
        emit(0, "//");
        emit(0, "// Generated by zcm-gen\n");
    }

    void emitComment(int indent, const string& comment)
    {
        if (comment == "")
            return;

        auto lines = StringUtil::split(comment, '\n');
        if (lines.size() == 1) {
            emit(indent, "/// %s", lines[0].c_str());
        } else {
            emit(indent, "/**");
            for (auto& line : lines) {
                if (line.size() > 0) {
                    emit(indent, " * %s", line.c_str());
                } else {
                    emit(indent, " *");
                }
            }
            emit(indent, " */");
        }
    }
};

struct EmitHeader : public Emit
{
    EmitHeader(ZCMGen& zcm, ZCMStruct& lr, const string& fname):
        Emit{zcm, lr, fname} {}

    /** Emit output that is common to every header file **/
    void emitHeaderTop()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0, "#include <stdint.h>");
        emit(0, "#include <stdlib.h>");
        emit(0, "#include <zcm/zcm_coretypes.h>");

        if(!zcm.gopt->getBool("c-no-pubsub")) {
            emit(0, "#include <zcm/zcm.h>");
        }
        emit(0, "");

        emit(0, "#ifndef _%s_h", tn_);
        emit(0, "#define _%s_h", tn_);
        emit(0, "");

        emit(0, "#ifdef __cplusplus");
        emit(0, "extern \"C\" {");
        emit(0, "#endif");
        emit(0, "");

    }

    /** Emit output that is common to every header file **/
    void emitHeaderBottom()
    {
        emit(0, "#ifdef __cplusplus");
        emit(0, "}");
        emit(0, "#endif");
        emit(0, "");
        emit(0, "#endif");
    }

    /** Emit header file output specific to a particular type of struct. **/
    void emitHeaderStruct()
    {
        string tn = dotsToUnderscores(lr.structname.fullname);
        string tnUpper = StringUtil::toUpper(tn);

        // include header files required by members
        for (auto& lm : lr.members) {
            if (!ZCMGen::isPrimitiveType(lm.type.fullname) &&
                lm.type.fullname != lr.structname.fullname) {
                string otherTn = dotsToUnderscores(lm.type.fullname);
                emit(0, "#include \"%s%s%s.h\"",
                     zcm.gopt->getString("c-include").c_str(),
                     zcm.gopt->getString("c-include").size()>0 ? "/" : "",
                     otherTn.c_str());
            }
        }

        // output constants
        for (auto& lc : lr.constants) {
            assert(ZCMGen::isLegalConstType(lc.type));
            string suffix = (lc.type == "int64_t") ? "LL" : "";
            emitComment(0, lc.comment.c_str());
            emit(0, "#define %s_%s %s%s", tnUpper.c_str(),
                 lc.membername.c_str(), lc.valstr.c_str(), suffix.c_str());
        }
        if (lr.constants.size() > 0)
            emit(0, "");

        // define the struct
        emitComment(0, lr.comment.c_str());
        emit(0, "typedef struct _%s %s;", tn.c_str(), tn.c_str());
        emit(0, "struct _%s", tn.c_str());
        emit(0, "{");

        for (auto& lm : lr.members) {
            emitComment(1, lm.comment.c_str());

            int ndim = lm.dimensions.size();
            if (ndim == 0) {
                emit(1, "%-10s %s;", mapTypeName(lm.type.fullname).c_str(), lm.membername.c_str());
            } else {
                if (lm.isConstantSizeArray()) {
                    emitStart(1, "%-10s %s", mapTypeName(lm.type.fullname).c_str(), lm.membername.c_str());
                    for (auto& ld : lm.dimensions) {
                        emitContinue("[%s]", ld.size.c_str());
                    }
                    emitEnd(";");
                } else {
                    emitStart(1, "%-10s ", mapTypeName(lm.type.fullname).c_str());
                    for (int d = 0; d < ndim; d++)
                        emitContinue("*");
                    emitEnd("%s;", lm.membername.c_str());
                }
            }
        }
        emit(0, "};");
        emit(0, "");
    }

    void emitHeaderPrototypes()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0, "/**");
        emit(0, " * Create a deep copy of a %s.", tn_);
        emit(0, " * When no longer needed, destroy it with %s_destroy()", tn_);
        emit(0, " */");
        emit(0,"%s* %s_copy(const %s* to_copy);", tn_, tn_, tn_);
        emit(0, "");
        emit(0, "/**");
        emit(0, " * Destroy an instance of %s created by %s_copy()", tn_, tn_);
        emit(0, " */");
        emit(0,"void %s_destroy(%s* to_destroy);", tn_, tn_);
        emit(0,"");

        if (!zcm.gopt->getBool("c-no-pubsub")) {
            emit(0, "/**");
            emit(0, " * Identifies a single subscription.  This is an opaque data type.");
            emit(0, " */");
            emit(0,"typedef struct _%s_subscription_t %s_subscription_t;", tn_, tn_);
            emit(0, "");
            emit(0, "/**");
            emit(0, " * Prototype for a callback function invoked when a message of type");
            emit(0, " * %s is received.", tn_);
            emit(0, " */");
            emit(0,"typedef void(*%s_handler_t)(const zcm_recv_buf_t *rbuf,\n"
                 "             const char *channel, const %s *msg, void *userdata);",
                 tn_, tn_);
            emit(0, "");
            emit(0, "/**");
            emit(0, " * Publish a message of type %s using ZCM.", tn_);
            emit(0, " *");
            emit(0, " * @param zcm The ZCM instance to publish with.");
            emit(0, " * @param channel The channel to publish on.");
            emit(0, " * @param msg The message to publish.");
            emit(0, " * @return 0 on success, <0 on error.  Success means ZCM has transferred");
            emit(0, " * responsibility of the message data to the OS.");
            emit(0, " */");
            emit(0,"int %s_publish(zcm_t *zcm, const char *channel, const %s *msg);", tn_, tn_);
            emit(0, "");
            emit(0, "/**");
            emit(0, " * Subscribe to messages of type %s using ZCM.", tn_);
            emit(0, " *");
            emit(0, " * @param zcm The ZCM instance to subscribe with.");
            emit(0, " * @param channel The channel to subscribe to.");
            emit(0, " * @param handler The callback function invoked by ZCM when a message is received.");
            emit(0, " *                This function is invoked by ZCM during calls to zcm_handle() and");
            emit(0, " *                zcm_handle_timeout().");
            emit(0, " * @param userdata An opaque pointer passed to @p handler when it is invoked.");
            emit(0, " * @return pointer to subscription type, NULL if failure. Must clean up");
            emit(0, " *         dyanic memory by passing the pointer to %s_unsubscribe.", tn_);
            emit(0, " */");
            emit(0,"%s_subscription_t* %s_subscribe(zcm_t *zcm, const char *channel, %s_handler_t handler, void *userdata);",
                 tn_, tn_, tn_);
            emit(0, "");
            emit(0, "/**");
            emit(0, " * Removes and destroys a subscription created by %s_subscribe()", tn_);
            emit(0, " */");
            emit(0,"int %s_unsubscribe(zcm_t *zcm, %s_subscription_t* hid);", tn_, tn_);
            //emit(0, "");
            //emit(0, "/**");
            //emit(0, " * Sets the queue capacity for a subscription.");
            //emit(0, " * Some ZCM providers (e.g., the default multicast provider) are implemented");
            //emit(0, " * using a background receive thread that constantly revceives messages from");
            //emit(0, " * the network.  As these messages are received, they are buffered on");
            //emit(0, " * per-subscription queues until dispatched by zcm_handle().  This function");
            //emit(0, " * how many messages are queued before dropping messages.");
            //emit(0, " *");
            //emit(0, " * @param subs the subscription to modify.");
            //emit(0, " * @param num_messages The maximum number of messages to queue");
            //emit(0, " *  on the subscription.");
            //emit(0, " * @return 0 on success, <0 if an error occured");
            //emit(0, " */");
            //emit(0,"int %s_subscription_set_queue_capacity(%s_subscription_t* subs,\n"
                   //"                              int num_messages);\n", tn_, tn_);
        }

        emit(0, "/**");
        emit(0, " * Encode a message of type %s into binary form.", tn_);
        emit(0, " *");
        emit(0, " * @param buf The output buffer.");
        emit(0, " * @param offset Encoding starts at this byte offset into @p buf.");
        emit(0, " * @param maxlen Maximum number of bytes to write.  This should generally");
        emit(0, " *               be equal to %s_encoded_size().", tn_);
        emit(0, " * @param msg The message to encode.");
        emit(0, " * @return The number of bytes encoded, or <0 if an error occured.");
        emit(0, " */");
        emit(0,"int %s_encode(void *buf, int offset, int maxlen, const %s *p);", tn_, tn_);
        emit(0, "");
        emit(0, "/**");
        emit(0, " * Decode a message of type %s from binary form.", tn_);
        emit(0, " * When decoding messages containing strings or variable-length arrays, this");
        emit(0, " * function may allocate memory.  When finished with the decoded message,");
        emit(0, " * release allocated resources with %s_decode_cleanup().", tn_);
        emit(0, " *");
        emit(0, " * @param buf The buffer containing the encoded message");
        emit(0, " * @param offset The byte offset into @p buf where the encoded message starts.");
        emit(0, " * @param maxlen The maximum number of bytes to read while decoding.");
        emit(0, " * @param msg Output parameter where the decoded message is stored");
        emit(0, " * @return The number of bytes decoded, or <0 if an error occured.");
        emit(0, " */");
        emit(0,"int %s_decode(const void *buf, int offset, int maxlen, %s *msg);", tn_, tn_);
        emit(0, "");
        emit(0, "/**");
        emit(0, " * Release resources allocated by %s_decode()", tn_);
        emit(0, " * @return 0");
        emit(0, " */");
        emit(0,"int %s_decode_cleanup(%s *p);", tn_, tn_);
        emit(0, "");
        emit(0, "/**");
        emit(0, " * Check how many bytes are required to encode a message of type %s", tn_);
        emit(0, " */");
        emit(0,"int %s_encoded_size(const %s *p);", tn_, tn_);
        if(zcm.gopt->getBool("c-typeinfo")) {
            emit(0,"size_t %s_struct_size(void);", tn_);
            emit(0,"int  %s_num_fields(void);", tn_);
            emit(0,"int  %s_get_field(const %s *p, int i, zcm_field_t *f);", tn_, tn_);
            emit(0,"const zcm_type_info_t *%s_get_type_info(void);", tn_);
        }
        emit(0,"");

        emit(0,"// ZCM support functions. Users should not call these");
        emit(0,"int64_t __%s_get_hash(void);", tn_);
        emit(0,"int64_t __%s_hash_recursive(const __zcm_hash_ptr *p);", tn_);
        emit(0,"int     __%s_encode_array(void *buf, int offset, int maxlen, const %s *p, int elements);", tn_, tn_);
        emit(0,"int     __%s_decode_array(const void *buf, int offset, int maxlen, %s *p, int elements);", tn_, tn_);
        emit(0,"int     __%s_decode_array_cleanup(%s *p, int elements);", tn_, tn_);
        emit(0,"int     __%s_encoded_array_size(const %s *p, int elements);", tn_, tn_);
        emit(0,"int     __%s_clone_array(const %s *p, %s *q, int elements);", tn_, tn_, tn_);
        emit(0,"");
    }
};

struct EmitSource : public Emit
{
    EmitSource(ZCMGen& zcm, ZCMStruct& lr, const string& fname):
        Emit{zcm, lr, fname} {}

    void emitIncludes()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();
        emit(0, "#include <string.h>");
        emit(0, "#include \"%s%s%s.h\"",
                zcm.gopt->getString("c-include").c_str(),
                zcm.gopt->getString("c-include").size()>0 ? "/" : "",
                tn_);
        emit(0, "");
    }

    void emitCStructGetHash()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0, "static int __%s_hash_computed;", tn_);
        emit(0, "static int64_t __%s_hash;", tn_);
        emit(0, "");

        emit(0, "int64_t __%s_hash_recursive(const __zcm_hash_ptr *p)", tn_);
        emit(0, "{");
        emit(1,     "const __zcm_hash_ptr *fp;");
        emit(1,     "for (fp = p; fp != NULL; fp = fp->parent)");
        emit(2,         "if (fp->v == __%s_get_hash)", tn_);
        emit(3,              "return 0;");
        emit(0, "");
        emit(1, "__zcm_hash_ptr cp;");
        emit(1, "cp.parent =  p;");
        emit(1, "cp.v = (void*)__%s_get_hash;", tn_);
        emit(1, "(void) cp;");
        emit(0, "");
        emit(1, "int64_t hash = (int64_t)0x%016" PRIx64 "LL", lr.hash);

        for (auto& lm : lr.members)
            emit(2, " + __%s_hash_recursive(&cp)", dotsToUnderscores(lm.type.fullname).c_str());

        emit(2,";");
        emit(0, "");
        emit(1, "return (hash<<1) + ((hash>>63)&1);");
        emit(0, "}");
        emit(0, "");

        emit(0, "int64_t __%s_get_hash(void)", tn_);
        emit(0, "{");
        emit(1, "if (!__%s_hash_computed) {", tn_);
        emit(2,      "__%s_hash = __%s_hash_recursive(NULL);", tn_, tn_);
        emit(2,      "__%s_hash_computed = 1;", tn_);
        emit(1,      "}");
        emit(0, "");
        emit(1, "return __%s_hash;", tn_);
        emit(0, "}");
        emit(0, "");
    }

    void emitCArrayLoopsStart(ZCMMember& lm, const string& n, int flags)
    {
        if (lm.dimensions.size() == 0)
            return;

        for (uint i = 0; i < lm.dimensions.size() - 1; i++) {
            char var = 'a' + i;

            if (flags & FLAG_EMIT_MALLOCS) {
                string stars = string(lm.dimensions.size()-1-i, '*');
                emit(2+i, "%s = (%s%s*) zcm_malloc(sizeof(%s%s) * %s);",
                     makeAccessor(lm, n, i).c_str(),
                     mapTypeName(lm.type.fullname).c_str(),
                     stars.c_str(),
                     mapTypeName(lm.type.fullname).c_str(),
                     stars.c_str(),
                     makeArraySize(lm, n, i).c_str());
            }

            emit(2+i, "{ int %c;", var);
            emit(2+i, "for (%c = 0; %c < %s; %c++) {", var, var, makeArraySize(lm, "p", i).c_str(), var);
        }

        if (flags & FLAG_EMIT_MALLOCS) {
            emit(2 + (int)lm.dimensions.size() - 1, "%s = (%s*) zcm_malloc(sizeof(%s) * %s);",
                 makeAccessor(lm, n, lm.dimensions.size() - 1).c_str(),
                 mapTypeName(lm.type.fullname).c_str(),
                 mapTypeName(lm.type.fullname).c_str(),
                 makeArraySize(lm, n, lm.dimensions.size() - 1).c_str());
        }
    }

    void emitCArrayLoopsEnd(ZCMMember& lm, const string& n, int flags)
    {
        if (lm.dimensions.size() == 0)
            return;

        auto sz = lm.dimensions.size();
        for (uint i = 0; i < sz - 1; i++) {
            uint indent = sz - i;
            if (flags & FLAG_EMIT_FREES) {
                string accessor = makeAccessor(lm, "p", sz-1-i);
                emit(indent+1, "if (%s) free(%s);", accessor.c_str(), accessor.c_str());
            }
            emit(indent, "}");
            emit(indent, "}");
        }

        if (flags & FLAG_EMIT_FREES) {
            string accessor = makeAccessor(lm, "p", 0);
            emit(2, "if (%s) free(%s);", accessor.c_str(), accessor.c_str());
        }
    }

    void emitCEncodeArray()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int __%s_encode_array(void *buf, int offset, int maxlen, const %s *p, int elements)", tn_, tn_);
        emit(0,"{");
        emit(1,    "int pos = 0, thislen, element;");
        emit(0,"");
        emit(1,    "for (element = 0; element < elements; element++) {");
        emit(0,"");
        for (auto& lm : lr.members) {
            emitCArrayLoopsStart(lm, "p", FLAG_NONE);

            int indent = 2+std::max(0, (int)lm.dimensions.size() - 1);
            emit(indent, "thislen = __%s_encode_array(buf, offset + pos, maxlen - pos, %s, %s);",
                 dotsToUnderscores(lm.type.fullname).c_str(),
                 makeAccessor(lm, "p", lm.dimensions.size()-1).c_str(),
                 makeArraySize(lm, "p", lm.dimensions.size()-1).c_str());
            emit(indent, "if (thislen < 0) return thislen; else pos += thislen;");

            emitCArrayLoopsEnd(lm, "p", FLAG_NONE);
            emit(0,"");
        }
        emit(1,   "}");
        emit(1, "return pos;");
        emit(0,"}");
        emit(0,"");
    }

    void emitCEncode()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int %s_encode(void *buf, int offset, int maxlen, const %s *p)", tn_, tn_);
        emit(0,"{");
        emit(1,    "int pos = 0, thislen;");
        emit(1,    "int64_t hash = __%s_get_hash();", tn_);
        emit(0,"");
        emit(1,    "thislen = __int64_t_encode_array(buf, offset + pos, maxlen - pos, &hash, 1);");
        emit(1,    "if (thislen < 0) return thislen; else pos += thislen;");
        emit(0,"");
        emit(1,    "thislen = __%s_encode_array(buf, offset + pos, maxlen - pos, p, 1);", tn_);
        emit(1,    "if (thislen < 0) return thislen; else pos += thislen;");
        emit(0,"");
        emit(1, "return pos;");
        emit(0,"}");
        emit(0,"");
    }

    void emitCDecodeArray()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int __%s_decode_array(const void *buf, int offset, int maxlen, %s *p, int elements)", tn_, tn_);
        emit(0,"{");
        emit(1,    "int pos = 0, thislen, element;");
        emit(0,"");
        emit(1,    "for (element = 0; element < elements; element++) {");
        emit(0,"");
        for (auto& lm : lr.members) {
            emitCArrayLoopsStart(lm, "p", lm.isConstantSizeArray() ? FLAG_NONE : FLAG_EMIT_MALLOCS);

            int indent = 2+std::max(0, (int)lm.dimensions.size() - 1);
            emit(indent, "thislen = __%s_decode_array(buf, offset + pos, maxlen - pos, %s, %s);",
                 dotsToUnderscores(lm.type.fullname).c_str(),
                 makeAccessor(lm, "p", lm.dimensions.size() - 1).c_str(),
                 makeArraySize(lm, "p", lm.dimensions.size() - 1).c_str());
            emit(indent, "if (thislen < 0) return thislen; else pos += thislen;");

            emitCArrayLoopsEnd(lm, "p", FLAG_NONE);
            emit(0,"");
        }
        emit(1,   "}");
        emit(1, "return pos;");
        emit(0,"}");
        emit(0,"");
    }

    void emitCDecodeArrayCleanup()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int __%s_decode_array_cleanup(%s *p, int elements)", tn_, tn_);
        emit(0,"{");
        emit(1,    "int element;");
        emit(1,    "for (element = 0; element < elements; element++) {");
        emit(0,"");
        for (auto& lm : lr.members) {
            emitCArrayLoopsStart(lm, "p", FLAG_NONE);

            int indent = 2+std::max(0, (int)lm.dimensions.size() - 1);
            emit(indent, "__%s_decode_array_cleanup(%s, %s);",
                 dotsToUnderscores(lm.type.fullname).c_str(),
                 makeAccessor(lm, "p", lm.dimensions.size() - 1).c_str(),
                 makeArraySize(lm, "p", lm.dimensions.size() - 1).c_str());

            emitCArrayLoopsEnd(lm, "p", lm.isConstantSizeArray() ? FLAG_NONE : FLAG_EMIT_FREES);
            emit(0,"");
        }
        emit(1,   "}");
        emit(1, "return 0;");
        emit(0,"}");
        emit(0,"");
    }

    void emitCDecode()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int %s_decode(const void *buf, int offset, int maxlen, %s *p)", tn_, tn_);
        emit(0,"{");
        emit(1,    "int pos = 0, thislen;");
        emit(1,    "int64_t hash = __%s_get_hash();", tn_);
        emit(0,"");
        emit(1,    "int64_t this_hash;");
        emit(1,    "thislen = __int64_t_decode_array(buf, offset + pos, maxlen - pos, &this_hash, 1);");
        emit(1,    "if (thislen < 0) return thislen; else pos += thislen;");
        emit(1,    "if (this_hash != hash) return -1;");
        emit(0,"");
        emit(1,    "thislen = __%s_decode_array(buf, offset + pos, maxlen - pos, p, 1);", tn_);
        emit(1,    "if (thislen < 0) return thislen; else pos += thislen;");
        emit(0,"");
        emit(1, "return pos;");
        emit(0,"}");
        emit(0,"");
    }

    void emitCDecodeCleanup()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int %s_decode_cleanup(%s *p)", tn_, tn_);
        emit(0,"{");
        emit(1, "return __%s_decode_array_cleanup(p, 1);", tn_);
        emit(0,"}");
        emit(0,"");
    }

    void emitCEncodedArraySize()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int __%s_encoded_array_size(const %s *p, int elements)", tn_, tn_);
        emit(0,"{");
        emit(1,"int size = 0, element;");
        emit(1,    "for (element = 0; element < elements; element++) {");
        emit(0,"");
        for (auto& lm : lr.members) {
            emitCArrayLoopsStart(lm, "p", FLAG_NONE);

            int indent = 2+std::max(0, (int)lm.dimensions.size() - 1);
            emit(indent, "size += __%s_encoded_array_size(%s, %s);",
                 dotsToUnderscores(lm.type.fullname).c_str(),
                 makeAccessor(lm, "p", lm.dimensions.size() - 1).c_str(),
                 makeArraySize(lm, "p", lm.dimensions.size() - 1).c_str());

            emitCArrayLoopsEnd(lm, "p", FLAG_NONE);
            emit(0,"");
        }
        emit(1,"}");
        emit(1, "return size;");
        emit(0,"}");
        emit(0,"");
    }

    void emitCEncodedSize()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int %s_encoded_size(const %s *p)", tn_, tn_);
        emit(0,"{");
        emit(1, "return 8 + __%s_encoded_array_size(p, 1);", tn_);
        emit(0,"}");
        emit(0,"");
    }

    void emitCNumFields()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int %s_num_fields(void)", tn_);
        emit(0,"{");
        emit(1, "return %d;", (int)lr.members.size());
        emit(0,"}");
        emit(0,"");
    }

    void emitCStructSize()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"size_t %s_struct_size(void)", tn_);
        emit(0,"{");
        emit(1, "return sizeof(%s);", tn_);
        emit(0,"}");
        emit(0,"");
    }

    void emitCGetField()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int %s_get_field(const %s *p, int i, zcm_field_t *f)", tn_, tn_);
        emit(0,"{");
        emit(1,"if (0 > i || i >= %s_num_fields())", tn_);
        emit(2,"return 1;");
        emit(1,"");

        emit(1,"switch (i) {");
        emit(1,"");

        int num_fields = lr.members.size();
        for(int i = 0; i < num_fields; i++) {
            emit(2,"case %d: {", i);

            ZCMMember& m = lr.members[i];

            string typeval;
            if(ZCMGen::isPrimitiveType(m.type.shortname)) {
                typeval = "ZCM_FIELD_" + StringUtil::toUpper(m.type.shortname);
            } else {
                emit(3,"/* %s */", m.type.shortname.c_str());
                typeval = "ZCM_FIELD_USER_TYPE";
            }

            emit(3,"f->name = \"%s\";", m.membername.c_str());
            emit(3,"f->type = %s;", typeval.c_str());
            emit(3,"f->typestr = \"%s\";", m.type.shortname.c_str());

            int num_dim = m.dimensions.size();
            emit(3,"f->num_dim = %d;", num_dim);

            if(num_dim != 0) {

                for(int j = 0; j < num_dim; j++) {
                    ZCMDimension& d = m.dimensions[j];
                    if(d.mode == ZCM_VAR)
                        emit(3,"f->dim_size[%d] = p->%s;", j, d.size.c_str());
                    else
                        emit(3,"f->dim_size[%d] = %s;", j, d.size.c_str());
                }

                for(int j = 0; j < num_dim; j++) {
                    ZCMDimension& d = m.dimensions[j];
                    emit(3,"f->dim_is_variable[%d] = %d;", j, d.mode == ZCM_VAR);
                }

            }

            emit(3, "f->data = (void *) &p->%s;", m.membername.c_str());

            emit(3, "return 0;");
            emit(2,"}");
            emit(2,"");
        }
        emit(2,"default:");
        emit(3,"return 1;");
        emit(1,"}");
        emit(0,"}");
        emit(0,"");
    }

    void emitCGetTypeInfo()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"const zcm_type_info_t *%s_get_type_info(void)", tn_);
        emit(0,"{");
        emit(1,"static int init = 0;");
        emit(1,"static zcm_type_info_t typeinfo;");
        emit(1,"if (!init) {");
        emit(2,"typeinfo.encode         = (zcm_encode_t) %s_encode;", tn_);
        emit(2,"typeinfo.decode         = (zcm_decode_t) %s_decode;", tn_);
        emit(2,"typeinfo.decode_cleanup = (zcm_decode_cleanup_t) %s_decode_cleanup;", tn_);
        emit(2,"typeinfo.encoded_size   = (zcm_encoded_size_t) %s_encoded_size;", tn_);
        emit(2,"typeinfo.struct_size    = (zcm_struct_size_t)  %s_struct_size;", tn_);
        emit(2,"typeinfo.num_fields     = (zcm_num_fields_t) %s_num_fields;", tn_);
        emit(2,"typeinfo.get_field      = (zcm_get_field_t) %s_get_field;", tn_);
        emit(2,"typeinfo.get_hash       = (zcm_get_hash_t) __%s_get_hash;", tn_);
        emit(1,"}");
        emit(1,"");
        emit(1,"return &typeinfo;");
        emit(0,"}");
    }

    void emitCCloneArray()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"int __%s_clone_array(const %s *p, %s *q, int elements)", tn_, tn_, tn_);
        emit(0,"{");
        emit(1,    "int element;");
        emit(1,    "for (element = 0; element < elements; element++) {");
        emit(0,"");
        for (auto& lm : lr.members) {

            emitCArrayLoopsStart(lm, "q", lm.isConstantSizeArray() ? FLAG_NONE : FLAG_EMIT_MALLOCS);

            int indent = 2+std::max(0, (int)lm.dimensions.size() - 1);
            emit(indent, "__%s_clone_array(%s, %s, %s);",
                 dotsToUnderscores(lm.type.fullname).c_str(),
                 makeAccessor(lm, "p", lm.dimensions.size()-1).c_str(),
                 makeAccessor(lm, "q", lm.dimensions.size()-1).c_str(),
                 makeArraySize(lm, "p", lm.dimensions.size()-1).c_str());

            emitCArrayLoopsEnd(lm, "p", FLAG_NONE);
            emit(0,"");
        }
        emit(1,   "}");
        emit(1,   "return 0;");
        emit(0,"}");
        emit(0,"");
    }

    void emitCCopy()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"%s *%s_copy(const %s *p)", tn_, tn_, tn_);
        emit(0,"{");
        emit(1,    "%s *q = (%s*) malloc(sizeof(%s));", tn_, tn_, tn_);
        emit(1,    "__%s_clone_array(p, q, 1);", tn_);
        emit(1,    "return q;");
        emit(0,"}");
        emit(0,"");
    }

    void emitCDestroy()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0,"void %s_destroy(%s *p)", tn_, tn_);
        emit(0,"{");
        emit(1,    "__%s_decode_array_cleanup(p, 1);", tn_);
        emit(1,    "free(p);");
        emit(0,"}");
        emit(0,"");
    }

    void emitCStructPublish()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();
        emit(0, "int %s_publish(zcm_t *lc, const char *channel, const %s *p)", tn_, tn_);
        emit(0, "{");
        emit(0, "      int max_data_size = %s_encoded_size (p);", tn_);
        emit(0, "      uint8_t *buf = (uint8_t*) malloc (max_data_size);");
        emit(0, "      if (!buf) return -1;");
        emit(0, "      int data_size = %s_encode (buf, 0, max_data_size, p);", tn_);
        emit(0, "      if (data_size < 0) {");
        emit(0, "          free (buf);");
        emit(0, "          return data_size;");
        emit(0, "      }");
        emit(0, "      int status = zcm_publish (lc, channel, (char *)buf, (size_t)data_size);");
        emit(0, "      free (buf);");
        emit(0, "      return status;");
        emit(0, "}");
        emit(0, "");
    }


    void emitCStructSubscribe()
    {
        string tmp_ = dotsToUnderscores(lr.structname.fullname);
        char *tn_ = (char *)tmp_.c_str();

        emit(0, "struct _%s_subscription_t {", tn_);
        emit(0, "    %s_handler_t user_handler;", tn_);
        emit(0, "    void *userdata;");
        emit(0, "    zcm_sub_t *z_sub;");
        emit(0, "};");
        emit(0, "static");
        emit(0, "void %s_handler_stub (const zcm_recv_buf_t *rbuf,", tn_);
        emit(0, "                            const char *channel, void *userdata)");
        emit(0, "{");
        emit(0, "    int status;");
        emit(0, "    %s p;", tn_);
        emit(0, "    memset(&p, 0, sizeof(%s));", tn_);
        emit(0, "    status = %s_decode (rbuf->data, 0, rbuf->data_size, &p);", tn_);
        emit(0, "    if (status < 0) {");
        emit(0, "        fprintf (stderr, \"error %%d decoding %s!!!\\n\", status);", tn_);
        emit(0, "        return;");
        emit(0, "    }");
        emit(0, "");
        emit(0, "    %s_subscription_t *h = (%s_subscription_t*) userdata;", tn_, tn_);
        emit(0, "    h->user_handler (rbuf, channel, &p, h->userdata);");
        emit(0, "");
        emit(0, "    %s_decode_cleanup (&p);", tn_);
        emit(0, "}");
        emit(0, "");
        emit(0, "%s_subscription_t* %s_subscribe (zcm_t *zcm,", tn_, tn_);
        emit(0, "                    const char *channel,");
        emit(0, "                    %s_handler_t f, void *userdata)", tn_);
        emit(0, "{");
        // TODO: it would be nice if this didn't need to malloc, currently only typed subscriptions
        //       in C allocate memory that isn't automatically cleaned up on the destruction of the
        //       zcm object.
        emit(0, "    %s_subscription_t *n = (%s_subscription_t*)", tn_, tn_);
        emit(0, "                       malloc(sizeof(%s_subscription_t));", tn_);
        emit(0, "    n->user_handler = f;");
        emit(0, "    n->userdata = userdata;");
        emit(0, "    n->z_sub = zcm_subscribe (zcm, channel,");
        emit(0, "                              %s_handler_stub, n);", tn_);
        emit(0, "    if (n->z_sub == NULL) {");
        emit(0, "        fprintf (stderr,\"couldn't reg %s ZCM handler!\\n\");", tn_);
        emit(0, "        free (n);");
        emit(0, "        return NULL;");
        emit(0, "    }");
        emit(0, "    return n;");
        emit(0, "}");
        //emit(0, "");
        //emit(0, "int %s_subscription_set_queue_capacity (%s_subscription_t* subs,", tn_, tn_);
        //emit(0, "                              int num_messages)");
        //emit(0, "{");
        //emit(0, "    return 0;//zcm_subscription_set_queue_capacity (subs->z_sub, num_messages);");
        //emit(0, "}\n");
        emit(0, "");
        emit(0, "int %s_unsubscribe(zcm_t *zcm, %s_subscription_t* hid)", tn_, tn_);
        emit(0, "{");
        emit(0, "    int status = zcm_unsubscribe (zcm, hid->z_sub);");
        emit(0, "    if (0 != status) {");
        emit(0, "        fprintf(stderr,");
        emit(0, "           \"couldn't unsubscribe %s_handler %%p!\\n\", hid);", tn_);
        emit(0, "        return -1;");
        emit(0, "    }");
        emit(0, "    free (hid);");
        emit(0, "    return 0;");
        emit(0, "}\n");
    }
};

static int emitStructHeader(ZCMGen& zcm, ZCMStruct& lr, const string& fname)
{
    EmitHeader E{zcm, lr, fname};
    if (!E.good())
        return -1;

    E.emitAutoGeneratedWarning();
    E.emitHeaderTop();
    E.emitHeaderStruct();
    E.emitHeaderPrototypes();

    E.emitHeaderBottom();
    return 0;
}

static int emitStructSource(ZCMGen& zcm, ZCMStruct& lr, const string& fname)
{
    EmitSource E{zcm, lr, fname};
    if (!E.good())
        return -1;

    E.emitAutoGeneratedWarning();
    E.emitIncludes();

    E.emitCStructGetHash();
    E.emitCEncodeArray();
    E.emitCEncode();
    E.emitCEncodedArraySize();
    E.emitCEncodedSize();

    if(zcm.gopt->getBool("c-typeinfo")) {
        E.emitCStructSize();
        E.emitCNumFields();
        E.emitCGetField();
        E.emitCGetTypeInfo();
    }

    E.emitCDecodeArray();
    E.emitCDecodeArrayCleanup();
    E.emitCDecode();
    E.emitCDecodeCleanup();

    E.emitCCloneArray();
    E.emitCCopy();
    E.emitCDestroy();

    if(!zcm.gopt->getBool("c-no-pubsub")) {
        E.emitCStructPublish();
        E.emitCStructSubscribe();
    }

    return 0;
}

void setupOptionsC(GetOpt& gopt)
{
    gopt.addString(0, "c-cpath",    ".",      "Location for .c files");
    gopt.addString(0, "c-hpath",    ".",      "Location for .h files");
    gopt.addString(0, "c-include",   "",       "Generated #include lines reference this folder");
    gopt.addBool(0, "c-no-pubsub",   0,     "Do not generate _publish and _subscribe functions");
    gopt.addBool(0, "c-typeinfo",   0,      "Generate typeinfo functions for each type");
}

int emitC(ZCMGen& zcm)
{
    for (auto& lr : zcm.structs) {

        string headerName = zcm.gopt->getString("c-hpath") + "/" + lr.nameUnderscore() + ".h";
        string cName      = zcm.gopt->getString("c-cpath") + "/" + lr.nameUnderscore() + ".c";

        // STRUCT H file
        if (zcm.needsGeneration(lr.zcmfile, headerName)) {
            if (int ret = emitStructHeader(zcm, lr, headerName))
                return ret;
        }

        // STRUCT C file
        if (zcm.needsGeneration(lr.zcmfile, cName)) {
            if (int ret = emitStructSource(zcm, lr, cName))
                return ret;
        }
    }

    return 0;
}
