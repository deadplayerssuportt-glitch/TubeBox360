/* json.c - Implementacion del lector de JSON (ver json.h). C89. */

#include <stdlib.h>
#include <string.h>
#include "json.h"

#define BLOCK_NODES 4096
#define MAX_DEPTH   256

typedef struct NodeBlock
{
    struct NodeBlock *next;
    int               used;
    JsonNode          nodes[BLOCK_NODES];
} NodeBlock;

typedef struct
{
    char      *p;       /* posicion actual en el texto */
    NodeBlock *blocks;  /* bloques de nodos reservados */
    int        error;
} Parser;

static JsonNode *parse_value(Parser *ps, int depth);

/* ------------------------------------------------------------------ */

static JsonNode *new_node(Parser *ps, JsonType type)
{
    NodeBlock *b = ps->blocks;
    JsonNode *n;

    if (!b || b->used == BLOCK_NODES)
    {
        b = (NodeBlock *)malloc(sizeof(NodeBlock));
        if (!b)
        {
            ps->error = 1;
            return NULL;
        }
        b->next = ps->blocks;
        b->used = 0;
        ps->blocks = b;
    }
    n = &b->nodes[b->used++];
    memset(n, 0, sizeof(*n));
    n->type = type;
    return n;
}

static void skip_ws(Parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

static int hex4(const char *s, unsigned int *out)
{
    unsigned int v = 0;
    int i;
    char c;

    for (i = 0; i < 4; ++i)
    {
        c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned int)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *put_utf8(char *dst, unsigned int cp)
{
    if (cp < 0x80)
    {
        *dst++ = (char)cp;
    }
    else if (cp < 0x800)
    {
        *dst++ = (char)(0xC0 | (cp >> 6));
        *dst++ = (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        *dst++ = (char)(0xE0 | (cp >> 12));
        *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *dst++ = (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        *dst++ = (char)(0xF0 | (cp >> 18));
        *dst++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *dst++ = (char)(0x80 | (cp & 0x3F));
    }
    return dst;
}

/* ps->p apunta a la comilla inicial. Desescapa en el mismo buffer.
 * El resultado nunca es mas largo que el original, asi que cabe. */
static char *parse_string(Parser *ps)
{
    char *src = ps->p + 1;
    char *dst = src;
    char *start = src;
    unsigned int cp, lo;

    for (;;)
    {
        char c = *src;

        if (c == 0)
        {
            ps->error = 1;
            return NULL;
        }
        if (c == '"')
        {
            ps->p = src + 1;
            *dst = 0;
            return start;
        }
        if (c != '\\')
        {
            *dst++ = *src++;
            continue;
        }

        /* secuencia de escape */
        src++;
        switch (*src)
        {
        case '"':  *dst++ = '"';  src++; break;
        case '\\': *dst++ = '\\'; src++; break;
        case '/':  *dst++ = '/';  src++; break;
        case 'b':  *dst++ = '\b'; src++; break;
        case 'f':  *dst++ = '\f'; src++; break;
        case 'n':  *dst++ = '\n'; src++; break;
        case 'r':  *dst++ = '\r'; src++; break;
        case 't':  *dst++ = '\t'; src++; break;
        case 'u':
            if (!hex4(src + 1, &cp))
            {
                ps->error = 1;
                return NULL;
            }
            src += 5;
            /* par sustituto UTF-16 (emojis, etc.) */
            if (cp >= 0xD800 && cp <= 0xDBFF)
            {
                if (src[0] == '\\' && src[1] == 'u' && hex4(src + 2, &lo) &&
                    lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    src += 6;
                }
                else
                {
                    cp = 0xFFFD;
                }
            }
            else if (cp >= 0xDC00 && cp <= 0xDFFF)
            {
                cp = 0xFFFD;
            }
            dst = put_utf8(dst, cp);
            break;
        default:
            ps->error = 1;
            return NULL;
        }
    }
}

static JsonNode *parse_object(Parser *ps, int depth)
{
    JsonNode *obj = new_node(ps, JSON_OBJECT);
    JsonNode *last = NULL;
    JsonNode *val;
    char *key;

    if (!obj)
        return NULL;
    ps->p++;   /* { */
    skip_ws(ps);
    if (*ps->p == '}')
    {
        ps->p++;
        return obj;
    }

    for (;;)
    {
        skip_ws(ps);
        if (*ps->p != '"')
        {
            ps->error = 1;
            return NULL;
        }
        key = parse_string(ps);
        if (!key)
            return NULL;

        skip_ws(ps);
        if (*ps->p != ':')
        {
            ps->error = 1;
            return NULL;
        }
        ps->p++;

        val = parse_value(ps, depth + 1);
        if (!val)
            return NULL;
        val->key = key;

        if (last)
            last->next = val;
        else
            obj->child = val;
        last = val;

        skip_ws(ps);
        if (*ps->p == ',')
        {
            ps->p++;
            continue;
        }
        if (*ps->p == '}')
        {
            ps->p++;
            return obj;
        }
        ps->error = 1;
        return NULL;
    }
}

static JsonNode *parse_array(Parser *ps, int depth)
{
    JsonNode *arr = new_node(ps, JSON_ARRAY);
    JsonNode *last = NULL;
    JsonNode *val;

    if (!arr)
        return NULL;
    ps->p++;   /* [ */
    skip_ws(ps);
    if (*ps->p == ']')
    {
        ps->p++;
        return arr;
    }

    for (;;)
    {
        val = parse_value(ps, depth + 1);
        if (!val)
            return NULL;

        if (last)
            last->next = val;
        else
            arr->child = val;
        last = val;

        skip_ws(ps);
        if (*ps->p == ',')
        {
            ps->p++;
            continue;
        }
        if (*ps->p == ']')
        {
            ps->p++;
            return arr;
        }
        ps->error = 1;
        return NULL;
    }
}

static JsonNode *parse_value(Parser *ps, int depth)
{
    JsonNode *n;
    char *end;

    if (depth > MAX_DEPTH)
    {
        ps->error = 1;
        return NULL;
    }

    skip_ws(ps);
    switch (*ps->p)
    {
    case '{':
        return parse_object(ps, depth);
    case '[':
        return parse_array(ps, depth);
    case '"':
        n = new_node(ps, JSON_STRING);
        if (!n)
            return NULL;
        n->str = parse_string(ps);
        return n->str ? n : NULL;
    case 't':
        if (strncmp(ps->p, "true", 4) != 0)
            break;
        n = new_node(ps, JSON_BOOL);
        if (!n)
            return NULL;
        n->boolean = 1;
        ps->p += 4;
        return n;
    case 'f':
        if (strncmp(ps->p, "false", 5) != 0)
            break;
        n = new_node(ps, JSON_BOOL);
        if (!n)
            return NULL;
        ps->p += 5;
        return n;
    case 'n':
        if (strncmp(ps->p, "null", 4) != 0)
            break;
        n = new_node(ps, JSON_NULL);
        if (!n)
            return NULL;
        ps->p += 4;
        return n;
    default:
        if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9'))
        {
            n = new_node(ps, JSON_NUMBER);
            if (!n)
                return NULL;
            n->num = strtod(ps->p, &end);
            if (end == ps->p)
                break;
            ps->p = end;
            return n;
        }
        break;
    }

    ps->error = 1;
    return NULL;
}

/* ------------------------------------------------------------------ */

static void free_blocks(NodeBlock *b)
{
    NodeBlock *next;

    while (b)
    {
        next = b->next;
        free(b);
        b = next;
    }
}

JsonDoc *json_parse(char *text)
{
    Parser ps;
    JsonNode *root;
    JsonDoc *doc;

    if (!text)
        return NULL;

    ps.p = text;
    ps.blocks = NULL;
    ps.error = 0;

    root = parse_value(&ps, 0);
    if (!root || ps.error)
    {
        free_blocks(ps.blocks);
        return NULL;
    }

    doc = (JsonDoc *)malloc(sizeof(JsonDoc));
    if (!doc)
    {
        free_blocks(ps.blocks);
        return NULL;
    }
    doc->root = root;
    doc->blocks = ps.blocks;
    return doc;
}

void json_free(JsonDoc *doc)
{
    if (!doc)
        return;
    free_blocks((NodeBlock *)doc->blocks);
    free(doc);
}

JsonNode *json_get(const JsonNode *obj, const char *key)
{
    JsonNode *c;

    if (!obj || obj->type != JSON_OBJECT)
        return NULL;
    for (c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0)
            return c;
    return NULL;
}

JsonNode *json_at(const JsonNode *arr, int index)
{
    JsonNode *c;

    if (!arr || arr->type != JSON_ARRAY || index < 0)
        return NULL;
    for (c = arr->child; c && index > 0; c = c->next)
        --index;
    return c;
}

JsonNode *json_path(const JsonNode *node, const char *path)
{
    char part[128];
    const char *dot;
    size_t len;
    JsonNode *cur = (JsonNode *)node;

    while (cur && *path)
    {
        dot = strchr(path, '.');
        len = dot ? (size_t)(dot - path) : strlen(path);
        if (len >= sizeof(part))
            return NULL;
        memcpy(part, path, len);
        part[len] = 0;

        if (cur->type == JSON_ARRAY && part[0] >= '0' && part[0] <= '9')
            cur = json_at(cur, atoi(part));
        else
            cur = json_get(cur, part);

        path += len;
        if (*path == '.')
            path++;
    }
    return cur;
}

const char *json_str(const JsonNode *node)
{
    return (node && node->type == JSON_STRING) ? node->str : NULL;
}
