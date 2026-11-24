/* net.h - Red de la Xbox: arranque de XNet y peticiones HTTP/HTTPS.
 *
 * Fase 2 (actual): la consola habla HTTPS directo con YouTube (mbedTLS).
 * El modo proxy de la fase 1 (la PC con ProtoHTTPS) sigue disponible con
 * conexion=proxy en game:\config.ini. El resto de la app no cambia: pide
 * URLs https:// normales con http_request. */
#ifndef NET_H
#define NET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Direccion del proxy (la PC). Cambiala si tu PC tiene otra IP,
 * por ejemplo cuando uses la RGH en la red de tu casa. */
#define PROXY_IP    "192.168.50.1"
#define PROXY_PORT  8080

typedef struct
{
    int   status;    /* codigo HTTP (200, 404...), 0 si no hubo respuesta */
    char *data;      /* respuesta completa, terminada en 0 (uso interno) */
    char *body;      /* apunta al cuerpo dentro de data, terminado en 0 */
    int   bodyLen;
} HttpResponse;

/* Arranca la pila de red. Llamar una vez al inicio. 1 = bien. */
int  net_init(void);

/* Hace una peticion (HTTPS directo o a traves del proxy, segun el modo).
 *   method:       "GET" o "POST"
 *   url:          URL completa, p. ej. "https://www.youtube.com/..."
 *   extraHeaders: lineas "Nombre: valor\r\n" adicionales, o NULL
 *   body/bodyLen: cuerpo del POST, o NULL/0
 * Devuelve 1 si llego una respuesta (mira resp->status). En error devuelve
 * 0 y deja un mensaje en err. Libera la respuesta con http_free. */
int  http_request(const char *method, const char *url, const char *extraHeaders,
                  const char *body, int bodyLen, HttpResponse *resp,
                  char *err, int errSize);

void http_free(HttpResponse *resp);

/* Proxy de la PC (fase 1). Por omision PROXY_IP:PROXY_PORT; se puede
 * cambiar en game:\config.ini con  proxy=192.168.1.70:8080 */
void net_set_proxy(const char *ip, int port);
const char *net_get_proxy(int *port);

/* Modo de conexion: 1 = HTTPS directo (por omision), 0 = proxy de la PC.
 * En game:\config.ini:  conexion=directa  o  conexion=proxy */
/* Bandera de cancelar del hilo que llama (NULL = ninguna). Si *flag pasa a
 * distinto de 0, la peticion en curso de ese hilo termina en menos de
 * medio segundo con el error "Cancelado". La usa el reproductor con g_stop. */
void net_set_cancel(volatile long *flag);

void net_set_direct(int on);
/* Verificar certificados HTTPS con las raices de Google (tls_roots.c).
 * Por omision si; config.ini certificados=0 lo quita (solo para probar). */
void net_set_verify(int on);
int  net_verify_on(void);       /* 1 = se estan verificando */
int  net_is_direct(void);

/* Bitacora en game:\tubebox_red.txt (en la RGH no hay depurador): agrega
 * una linea con la hora. Se puede llamar desde cualquier hilo. */
void net_log(const char *fmt, ...);

/* Prueba de red SIN la PC (cable/wifi -> internet): enlace e IP de la
 * consola, DNS, TCP al 443 de YouTube, HTTP simple a
 * connectivitycheck.gstatic.com (espera 204), numeros aleatorios, HTTPS a
 * www.youtube.com/generate_204 y una descarga de prueba con su velocidad. Escribe el reporte (varias lineas) en out y en la bitacora.
 * Devuelve cuantos pasos fallaron (0 = todo bien). Bloquea unos segundos:
 * llamala desde un hilo aparte. */
int  net_selftest(char *out, int size);

#ifdef __cplusplus
}
#endif

#endif /* NET_H */
