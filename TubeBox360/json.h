/* json.h - Lector de JSON pequeño, en C89.
 *
 * Lee el texto "en su lugar": las cadenas se desescapan dentro del mismo
 * buffer que le pasas, asi que ese buffer se modifica y debe seguir vivo
 * mientras uses el documento. No copia cadenas; solo reserva nodos.
 *
 * Uso:
 *   JsonDoc *doc = json_parse(texto);
 *   if (doc) {
 *       const char *t = json_str(json_path(doc->root, "a.b.0.c"));
 *       json_free(doc);
 *   } */
#ifndef JSON_H
#define JSON_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonNode
{
    JsonType         type;
    const char      *key;     /* nombre si es miembro de un objeto, si no NULL */
    const char      *str;     /* valor si es cadena (UTF-8 ya desescapado) */
    double           num;     /* valor si es numero */
    int              boolean; /* valor si es true/false */
    struct JsonNode *child;   /* primer hijo (arreglo u objeto) */
    struct JsonNode *next;    /* siguiente hermano */
} JsonNode;

typedef struct JsonDoc
{
    JsonNode *root;
    void     *blocks;         /* uso interno */
} JsonDoc;

/* Devuelve NULL si el texto no es JSON valido o falta memoria. */
JsonDoc    *json_parse(char *text);
void        json_free(JsonDoc *doc);

/* Miembro de un objeto por nombre (NULL si no existe). */
JsonNode   *json_get(const JsonNode *obj, const char *key);

/* Elemento de un arreglo por posicion (NULL si no existe). */
JsonNode   *json_at(const JsonNode *arr, int index);

/* Ruta separada por puntos; los numeros indexan arreglos: "a.runs.0.text" */
JsonNode   *json_path(const JsonNode *node, const char *path);

/* Cadena del nodo, o NULL si no es cadena. */
const char *json_str(const JsonNode *node);

#ifdef __cplusplus
}
#endif

#endif /* JSON_H */
