/* version.h - Version de TubeBox360 y modo beta. */
#ifndef TB_VERSION_H
#define TB_VERSION_H

#define TB_VERSION   "1.00"
/* Numero de version para el aviso de actualizacion: 100 = 1.00, 101 = 1.01...
 * Subelo en cada version que publiques (y el "build" de Firebase igual). */
#define TB_BUILD     100

/* TB_BETA 1 = version para usuarios (la que se publica): sin archivos de
 * depuracion (debug_*.json) ni las pruebas de desarrollo de la pantalla de
 * formatos. La prueba de red y los reportes de errores SI quedan.
 * Ponlo en 0 solo para volver a tener las herramientas de desarrollo. */
#define TB_BETA      1

/* Contador de "Usuarios activos" (menu lateral): direccion de tu base de
 * Firebase Realtime Database, sin "/" al final. Vacia = no se usa.
 * Ejemplo: "https://tubebox360-default-rtdb.firebaseio.com" */
#ifndef TB_PRESENCE_URL
#define TB_PRESENCE_URL  ""
#endif

/* Cuenta de Google (suscripciones y me gusta). Del proyecto de Google
 * Cloud del usuario: cliente OAuth tipo "TVs and Limited Input devices".
 * Vacios = la app funciona igual, pero no se puede iniciar sesion.
 * (Con este tipo de cliente Google no considera el secreto como secreto
 * real, pero no publiques tu version.h con ellos puestos.) */
#ifndef TB_GOOGLE_CLIENT_ID
#define TB_GOOGLE_CLIENT_ID      ""
#endif
#ifndef TB_GOOGLE_CLIENT_SECRET
#define TB_GOOGLE_CLIENT_SECRET  ""
#endif

#endif /* TB_VERSION_H */
