# Genera lang_tab.h: X(ID, "es", "en", "pt") con los acentos como \x.. de UTF-8
# (cortando la cadena despues de cada escape, para que una letra hexadecimal
# que siga no se la trague el escape).
import sys
S = [
# menu lateral
('S_SEC_SEARCH', 'Buscar', 'Search', 'Pesquisar'),
('S_SEC_HOME', 'Inicio', 'Home', 'Início'),
('S_SEC_SHORTS', 'Shorts', 'Shorts', 'Shorts'),
('S_SEC_GAMING', 'Gaming', 'Gaming', 'Jogos'),
('S_SEC_MUSIC', 'Música', 'Music', 'Música'),
('S_SEC_NEWS', 'Noticias', 'News', 'Notícias'),
('S_SEC_SPORTS', 'Deportes', 'Sports', 'Esportes'),
('S_SEC_HISTORY', 'Historial', 'History', 'Histórico'),
('S_SEC_SETTINGS', 'Configuración', 'Settings', 'Configurações'),
('S_PICK_PROFILE', 'Elegir perfil', 'Choose profile', 'Escolher perfil'),
# Inicio
('S_BECAUSE', 'Porque viste: %s', 'Because you watched: %s', 'Porque você assistiu: %s'),
('S_TRENDING', 'Tendencias', 'Trending', 'Em alta'),
('S_TREND_QUERY', 'tendencias', 'trending', 'em alta'),
# busquedas de respaldo de las categorias
('S_Q_SHORTS', '#shorts', '#shorts', '#shorts'),
('S_Q_GAMING', 'videojuegos gameplay', 'video games gameplay', 'jogos gameplay'),
('S_Q_MUSIC', 'música', 'music', 'música'),
('S_Q_NEWS', 'noticias hoy', 'news today', 'notícias hoje'),
('S_Q_SPORTS', 'deportes resumen', 'sports highlights', 'esportes melhores momentos'),
# avisos de las listas
('S_NO_HOME', 'No llegaron recomendados. Presiona Y para buscar', 'No recommendations arrived. Press Y to search', 'Nenhuma recomendação chegou. Pressione Y para pesquisar'),
('S_NO_CAT', 'No llegaron videos de %s', 'No videos arrived from %s', 'Nenhum vídeo chegou de %s'),
('S_ERROR', 'Error: %s', 'Error: %s', 'Erro: %s'),
('S_NO_RESULTS', 'Sin resultados para "%s"', 'No results for "%s"', 'Nenhum resultado para "%s"'),
('S_NO_HISTORY', 'Todavía no has visto videos', "You haven't watched any videos yet", 'Você ainda não assistiu a nenhum vídeo'),
('S_SEARCHING_Q', 'Buscando "%s"', 'Searching "%s"', 'Pesquisando "%s"'),
('S_LOADING_REC', 'Cargando recomendados', 'Loading recommendations', 'Carregando recomendações'),
('S_LOADING_X', 'Cargando %s', 'Loading %s', 'Carregando %s'),
('S_N_VIDEOS', '%d videos', '%d videos', '%d vídeos'),
('S_SEARCHING', 'Buscando...', 'Searching...', 'Pesquisando...'),
('S_LOADING', 'Cargando', 'Loading', 'Carregando'),
('S_PRESS_Y', 'Presiona Y para buscar', 'Press Y to search', 'Pressione Y para pesquisar'),
('S_LOADING_CHANNEL', 'Cargando canal', 'Loading channel', 'Carregando canal'),
('S_NO_CHANNEL', 'Sin datos del canal', 'No channel data', 'Sem dados do canal'),
('S_CHANNEL_EMPTY', 'Este canal no tiene videos públicos', 'This channel has no public videos', 'Este canal não tem vídeos públicos'),
('S_LOAD_FAIL', 'No se pudo cargar el video', "The video couldn't be loaded", 'Não foi possível carregar o vídeo'),
('S_NO_QUALITY', 'Este video no tiene una calidad que la consola pueda reproducir', 'This video has no quality the console can play', 'Este vídeo não tem uma qualidade que o console possa reproduzir'),
('S_WAIT_TASK', 'Espera a que termine la tarea anterior', 'Wait for the previous task to finish', 'Aguarde a tarefa anterior terminar'),
('S_STOPPING', 'Deteniendo...', 'Stopping...', 'Parando...'),
('S_NET_INIT', 'Iniciando red...', 'Starting network...', 'Iniciando rede...'),
('S_NET_FAIL', 'No se pudo iniciar la red', "Couldn't start the network", 'Não foi possível iniciar a rede'),
('S_STARTING', 'Iniciando', 'Starting', 'Iniciando'),
# ayudas de botones
('H_WATCH', 'Ver', 'Watch', 'Assistir'),
('H_BACK', 'Volver', 'Back', 'Voltar'),
('H_PAGE', 'Página', 'Page', 'Página'),
('H_MENU', 'Menú', 'Menu', 'Menu'),
('H_CHANNEL', 'Canal', 'Channel', 'Canal'),
('H_CANCEL', 'Cancelar', 'Cancel', 'Cancelar'),
('H_EXIT', 'Salir', 'Exit', 'Sair'),
('H_SEARCH', 'Buscar', 'Search', 'Pesquisar'),
('H_TYPE', 'Escribir', 'Type', 'Digitar'),
('H_DELETE', 'Borrar', 'Delete', 'Apagar'),
('H_SPACE', 'Espacio', 'Space', 'Espaço'),
('H_OPEN_KB', 'Abrir el teclado', 'Open keyboard', 'Abrir teclado'),
('H_TO_LIST', 'Volver a la lista', 'Back to list', 'Voltar à lista'),
('H_ACCEPT', 'Aceptar', 'OK', 'OK'),
('H_CHOOSE', 'Elegir', 'Select', 'Escolher'),
('H_CHANGE', 'Cambiar', 'Change', 'Alterar'),
('H_OPEN', 'Abrir', 'Open', 'Abrir'),
('H_CLOSE', 'Cerrar', 'Close', 'Fechar'),
('H_RELOAD', 'Recargar', 'Reload', 'Recarregar'),
('H_CHANGE_PAGE', 'Cambiar de página', 'Change page', 'Mudar de página'),
# salir, carga y siguiente video
('S_YES', 'Sí', 'Yes', 'Sim'),
('S_NETTEST', 'Probar la conexión y crear reporte de errores', 'Test the connection and create an error report', 'Testar a conexão e criar relatório de erros'),
('S_NETTEST_RUN', 'Probando la conexión y creando el reporte...', 'Testing the connection and creating the report...', 'Testando a conexão e criando o relatório...'),
('S_REPORT_SAVED', 'Reporte guardado en %s. Envíalo por FTP junto con lo que estabas haciendo cuando falló.', 'Report saved to %s. Send it (via FTP) with what you were doing when it failed.', 'Relatório salvo em %s. Envie por FTP junto com o que você fazia quando falhou.'),
('S_REPORT_FAIL', 'No se pudo guardar el reporte de errores.', 'The error report could not be saved.', 'Não foi possível salvar o relatório de erros.'),
('S_NO', 'No', 'No', 'Não'),
('S_EXIT_Q', '¿Salir de TubeBox360?', 'Exit TubeBox360?', 'Sair do TubeBox360?'),
('S_LOADING_RECS', 'Cargando recomendaciones', 'Loading recommendations', 'Carregando recomendações'),
('S_LOADING_CATALOG', 'Cargando catálogo', 'Loading catalog', 'Carregando catálogo'),
('S_PLAY_NOW', 'Reproducir ahora', 'Play now', 'Reproduzir agora'),
('S_FINDING_NEXT', 'Buscando el siguiente video...', 'Looking for the next video...', 'Procurando o próximo vídeo...'),
('S_NEXT_IN', 'Siguiente video en %d', 'Next video in %d', 'Próximo vídeo em %d'),
# configuracion
('S_PROFILE_X', 'Perfil: %s', 'Profile: %s', 'Perfil: %s'),
('S_NO_SESSION', 'sin sesión', 'not signed in', 'sem sessão'),
('S_THEME_LIGHT', 'Tema: claro (sol)', 'Theme: light (sun)', 'Tema: claro (sol)'),
('S_THEME_DARK', 'Tema: oscuro (luna)', 'Theme: dark (moon)', 'Tema: escuro (lua)'),
('S_LANGUAGE', 'Idioma: Español', 'Language: English', 'Idioma: Português'),
('S_CONTROLS', 'Controles del mando', 'Controller guide', 'Controles do controle'),
# descripcion de Configuracion (texto del usuario, 2026-11-22; DeadPIayers va asi escrito)
('S_INFO1', 'TubeBox360: Cliente homebrew de YouTube para Xbox 360 desarrollado por @Vindaesh.',
            'TubeBox360: Homebrew YouTube client for Xbox 360 developed by @Vindaesh.',
            'TubeBox360: Cliente homebrew do YouTube para Xbox 360 desenvolvido por @Vindaesh.'),
('S_INFO2', 'Colaboradores de Desarrollo: @XvZoRraLoLAxV, @LukeGable & @CarlosLeyva en producción para la comunidad DeadPIayers',
            'Development collaborators: @XvZoRraLoLAxV, @LukeGable & @CarlosLeyva, in production for the DeadPIayers community',
            'Colaboradores de desenvolvimento: @XvZoRraLoLAxV, @LukeGable & @CarlosLeyva, em produção para a comunidade DeadPIayers'),
('S_INFO3', 'Creditos correspondientes: Video H.264 con el decodificador de la consola; la forma de usarlo la descubrió HG-ERIK (GPU-MEDIA-PLAYER-360).',
            'Credits: H.264 video through the console decoder; the way to use it was discovered by HG-ERIK (GPU-MEDIA-PLAYER-360).',
            'Créditos: vídeo H.264 com o decodificador do console; a forma de usá-lo foi descoberta por HG-ERIK (GPU-MEDIA-PLAYER-360).'),
# configuraciones predeterminadas y mas ajustes (2026-11-22)
('S_DEFAULTS_HDR', 'Configuraciones predeterminadas', 'Default settings', 'Configurações padrão'),
('S_DEF_AUDIO', 'Audio: %s', 'Audio: %s', 'Áudio: %s'),
('S_DEF_SUBS', 'Subtítulos: %s', 'Subtitles: %s', 'Legendas: %s'),
('S_DEF_QUALITY', 'Calidad: %s', 'Quality: %s', 'Qualidade: %s'),
('S_V_APPLANG', 'idioma de la app', 'app language', 'idioma do app'),
('S_V_ORIGINAL', 'original del video', "video's original", 'original do vídeo'),
('S_V_AUTO', 'automáticos', 'automatic', 'automáticas'),
('S_V_OFF', 'apagados', 'off', 'desativadas'),
('S_V_ES', 'español', 'Spanish', 'espanhol'),
('S_V_EN', 'inglés', 'English', 'inglês'),
('S_V_PT', 'portugués', 'Portuguese', 'português'),
('S_V_JA', 'japonés', 'Japanese', 'japonês'),
('S_V_AR', 'árabe', 'Arabic', 'árabe'),
('S_SOUNDS_X', 'Sonidos de la interfaz: %s', 'Interface sounds: %s', 'Sons da interface: %s'),
('S_AVATAR_X', 'Avatar en el menú lateral: %s', 'Avatar in the side menu: %s', 'Avatar no menu lateral: %s'),
('S_ON_LC', 'sí', 'on', 'sim'),
('S_OFF_LC2', 'no', 'off', 'não'),
('M_KB_BACK', 'Menú lateral', 'Side menu', 'Menu lateral'),
# teclado
('S_KB_SPACE', 'ESPACIO', 'SPACE', 'ESPAÇO'),
('S_KB_CLEAR', 'BORRAR', 'CLEAR', 'LIMPAR'),
('S_KB_SEARCH', 'BUSCAR', 'SEARCH', 'PESQUISAR'),
('S_NO_SUGG', 'Sin sugerencias por ahora', 'No suggestions yet', 'Sem sugestões por enquanto'),
('S_TYPE_TO_SEARCH', 'Escribe con el teclado para buscar', 'Type with the keyboard to search', 'Digite com o teclado para pesquisar'),
# reproductor
('S_CHANGING_TO', 'Cambiando a %s...', 'Switching to %s...', 'Mudando para %s...'),
('S_AUDIO_TO', 'Cambiando el audio a %s...', 'Switching audio to %s...', 'Mudando o áudio para %s...'),
('S_SEEKING', 'Buscando %s...', 'Going to %s...', 'Indo para %s...'),
('S_SUBS_X', 'Subtítulos: %s', 'Subtitles: %s', 'Legendas: %s'),
('S_SUBS_OFF', 'Subtítulos: apagados', 'Subtitles: off', 'Legendas: desativadas'),
('S_NO_CHAN_V', 'Este video no trae su canal', "This video doesn't include its channel", 'Este vídeo não traz o canal'),
('S_NO_SUBS', 'Este video no tiene subtítulos en los idiomas disponibles', 'This video has no subtitles in the available languages', 'Este vídeo não tem legendas nos idiomas disponíveis'),
('S_ONE_TRACK', 'Este video tiene una sola pista de audio', 'This video has only one audio track', 'Este vídeo tem só uma faixa de áudio'),
('S_AUDIO_X', 'Audio: %s', 'Audio: %s', 'Áudio: %s'),
('S_X_LOADING', '%s (cargando...)', '%s (loading...)', '%s (carregando...)'),
('S_QUALITY', 'Calidad', 'Quality', 'Qualidade'),
('S_SUBTITLES', 'Subtítulos', 'Subtitles', 'Legendas'),
('S_AUDIO_LANG', 'Idioma del audio', 'Audio language', 'Idioma do áudio'),
('S_OFF', 'Apagados', 'Off', 'Desativadas'),
('S_DEFAULT', 'Predeterminada', 'Default', 'Padrão'),
('S_REMAINING', 'Quedan: %s', 'Remaining: %s', 'Restam: %s'),
('S_CLOSE_VIDEO', 'Cerrar el video', 'Close video', 'Fechar o vídeo'),
('S_QUALITY_X', 'Calidad: %s', 'Quality: %s', 'Qualidade: %s'),
('S_GO_CHANNEL', 'Ir al canal', 'Go to channel', 'Ir ao canal'),
('S_OFF_LC', 'apagados', 'off', 'desativadas'),
('S_NONE', 'no hay', 'none', 'nenhuma'),
('S_AUDIO_LANG_X', 'Idioma del audio: %s', 'Audio language: %s', 'Idioma do áudio: %s'),
('S_SINGLE_TRACK', 'pista única', 'single track', 'faixa única'),
('S_SINGLE_TRACK_C', 'Pista única', 'Single track', 'Faixa única'),
('S_INFO_TIP', 'Información (datos del reproductor)', 'Information (player stats)', 'Informações (dados do player)'),
('S_AUTODUB', '%s (doblaje automático)', '%s (auto-dubbed)', '%s (dublagem automática)'),
('S_ORIGINAL', '%s (original)', '%s (original)', '%s (original)'),
('S_AUTOTRANS', '%s (traducción automática)', '%s (auto-translated)', '%s (tradução automática)'),
('S_LANG_ES', 'Español', 'Spanish', 'Espanhol'),
('S_LANG_EN', 'Inglés', 'English', 'Inglês'),
('S_LANG_PT', 'Portugués', 'Portuguese', 'Português'),
# manual de controles
('M_TITLE', 'Controles del mando', 'Controller guide', 'Controles do controle'),
('M_P_HOME', 'Inicio y listas', 'Home and lists', 'Início e listas'),
('M_P_KB', 'Teclado', 'Keyboard', 'Teclado'),
('M_P_PLAYER', 'Reproductor', 'Player', 'Player'),
('M_P_MENU', 'Menú lateral', 'Side menu', 'Menu lateral'),
('M_MOVE', 'Moverse', 'Move', 'Mover'),
('M_HOME_DPAD', 'Moverse por los videos', 'Move through the videos', 'Mover pelos vídeos'),
('M_GO_CHANNEL', 'Ir al canal', 'Go to the channel', 'Ir ao canal'),
('M_WATCH', 'Ver el video', 'Watch the video', 'Assistir ao vídeo'),
('M_RELOAD', 'Recargar el Inicio', 'Reload Home', 'Recarregar o Início'),
('M_SEARCH', 'Buscar', 'Search', 'Pesquisar'),
('M_PREV_PAGE', 'Página anterior', 'Previous page', 'Página anterior'),
('M_NEXT_PAGE', 'Página siguiente', 'Next page', 'Próxima página'),
('M_OPEN_MENU', 'Menú lateral', 'Side menu', 'Menu lateral'),
('M_EXIT', 'Salir de la app', 'Exit the app', 'Sair do app'),
('M_KB_DPAD', 'Moverse por el teclado', 'Move around the keyboard', 'Mover pelo teclado'),
('M_KB_A', 'Escribir la tecla', 'Type the key', 'Digitar a tecla'),
('M_KB_X', 'Borrar', 'Delete', 'Apagar'),
('M_KB_Y', 'Espacio', 'Space', 'Espaço'),
('M_KB_START', 'Buscar', 'Search', 'Pesquisar'),
('M_BACK', 'Volver', 'Back', 'Voltar'),
('M_PL_A', 'Pausa y controles', 'Pause and controls', 'Pausa e controles'),
('M_PL_DPAD', 'Izq/Der: ±10 s · Arriba/Abajo: controles', 'Left/Right: ±10 s · Up/Down: controls', 'Esq/Dir: ±10 s · Cima/Baixo: controles'),
('M_PL_LB', '–1 minuto', '–1 minute', '–1 minuto'),
('M_PL_RB', '+1 minuto', '+1 minute', '+1 minuto'),
('M_PL_Y', 'Siguiente pista de audio', 'Next audio track', 'Próxima faixa de áudio'),
('M_PL_X', 'Datos del reproductor', 'Player stats', 'Dados do player'),
('M_PL_B', 'Ocultar controles / salir', 'Hide controls / exit', 'Ocultar controles / sair'),
('M_MN_START', 'Abrir o cerrar el menú', 'Open or close the menu', 'Abrir ou fechar o menu'),
('M_MN_DPAD', 'Elegir la sección', 'Choose the section', 'Escolher a seção'),
('M_MN_X', 'Gesto del avatar (mascota, objeto)', 'Avatar gesture (pet, item)', 'Gesto do avatar (mascote, objeto)'),
('M_MN_A', 'Abrir la sección', 'Open the section', 'Abrir a seção'),
('M_MN_B', 'Cerrar el menú', 'Close the menu', 'Fechar o menu'),
('M_STICK', 'Palanca izq.', 'Left stick', 'Analógico esq.'),
('M_STICK_R', 'Palanca der.', 'Right stick', 'Analógico dir.'),
('M_MN_RS', 'Mover la cabeza del avatar', "Move the avatar's head", 'Mover a cabeça do avatar'),
('M_MN_LT', 'Girar el avatar a la izquierda', 'Turn the avatar left', 'Girar o avatar para a esquerda'),
('M_MN_RT', 'Girar el avatar a la derecha', 'Turn the avatar right', 'Girar o avatar para a direita'),
# avatar del menu lateral
('S_AV_HELP', 'Tu avatar', 'Your avatar', 'Seu avatar'),
('H_AV_HEAD', 'Mover la cabeza', 'Move head', 'Mover a cabeça'),
('H_AV_TURN', 'Girar', 'Turn', 'Girar'),
('H_AV_GESTURE', 'Gesto', 'Gesture', 'Gesto'),
('S_AV_NO_ITEM', 'No existen elementos interactivos en este avatar.', 'This avatar has no interactive items.', 'Este avatar não tem elementos interativos.'),
('S_AV_LOADING', 'Cargando avatar...', 'Loading avatar...', 'Carregando avatar...'),
('S_AV_SIGNIN', 'Inicia sesión para ver tu avatar', 'Sign in to see your avatar', 'Entre para ver seu avatar'),
]

def esc(t):
    out = '"'
    prev_hex = False
    for ch in t:
        b = ch.encode('utf-8')
        if len(b) == 1:
            c = ch
            if prev_hex and c in '0123456789abcdefABCDEF':
                out += '" "'
            if c == '"': c = '\\"'
            elif c == '\\': c = '\\\\'
            out += c
            prev_hex = False
        else:
            out += ''.join('\\x%02X' % v for v in b)
            prev_hex = True
    return out + '"'

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lang_ja_ar import JA_AR, NEW

ids = [s[0] for s in S] + [n[0] for n in NEW]
assert len(ids) == len(set(ids)), 'id repetido'
faltan = [s[0] for s in S if s[0] not in JA_AR]
assert not faltan, 'faltan en japones/arabe: %s' % faltan
import re
rows = [(i, es, en, pt) + JA_AR[i] for i, es, en, pt in S] + list(NEW)
for r in rows:
    fmts = [re.findall(r'%[0-9]*[sd]', t) for t in r[1:]]
    assert all(f == fmts[0] for f in fmts), 'formatos distintos en %s' % r[0]
lines = ['/* lang_tab.h - GENERADO por gen_lang.py: los textos de la app en espanol,',
         ' * ingles, portugues, japones y arabe. Se incluye dos veces (lang.h y',
         ' * lang.c) con X(). */']
for i, es, en, pt, ja, ar in rows:
    lines.append('X(%s, %s, %s, %s, %s, %s)' % (i, esc(es), esc(en), esc(pt), esc(ja), esc(ar)))
open(sys.argv[1], 'w', newline='\n').write('\n'.join(lines) + '\n')
print(len(rows), 'textos')
