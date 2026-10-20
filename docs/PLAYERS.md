# Forza Motorsport 2 — port nativo para PC

## Cómo empezar

1. Abre `Forza2Launcher.exe`. Arranca en inglés: pulsa **ES** arriba a la
   derecha para ponerlo en castellano.
2. Ve a **Instalación** (*Install*) → *Elegir ISO e importar…* y elige la imagen de disco
   (`.iso`) de tu copia de Forza Motorsport 2. Se extraen unos 5 GB en la
   carpeta `game\` y se comprueba cada archivo. Tarda uno o dos minutos.
3. En **Perfiles** (*Profiles*), crea tu perfil (tu "gamertag").
4. Revisa **Gráficos** y **Controles** si quieres, y pulsa **JUGAR**.

Necesitas una tarjeta gráfica compatible con Vulkan y los controladores al
día. La primera vez que juegas se preparan los shaders automáticamente.

Funciona con la versión europea y la americana del juego (title ID
4D5307EA). Si importas otra revisión, el launcher te avisa.

- **Disco extra de la edición limitada**: con el juego ya instalado, en
  **Instalación → Contenido descargable**, *Importar ISO del disco extra*.
  Añade los 8 packs de DLC premium y cada perfil los instala la próxima vez
  que juega.
- **Project Forza Plus 2** (solo con la versión americana): en la pestaña
  **Forza Plus 2**, elige el `.zip` del mod, el modo de trayectoria y pulsa
  *Instalar Project Forza Plus 2*. Después aparece la opción de aplicar el
  `.zip` de la actualización. Desde ahí se cambia de modo, se ajusta el campo
  de visión de cada cámara y se desinstala. El mod ya trae los coches y
  circuitos DLC, así que con él no hace falta el disco extra.

## Jugar online

En **Perfiles**, la tarjeta **Online** tiene *Jugar online* activado y un
punto verde cuando el servidor responde. En el juego entra en
**Multijugador → Interconexión** (*System Link*): verás todas las carreras
creadas en el servidor. Crea una o únete a una. No hace falta abrir puertos
en el router. Tu gamertag es tu nombre online.

Todos los jugadores tienen que usar la misma versión del port y, si usan
Project Forza Plus 2, la misma versión del mod.

## Controles con teclado (por defecto)

| Acción | Tecla |
|---|---|
| Acelerar (RT) | E |
| Frenar (LT) | Q |
| Dirección | A / D |
| Aceptar (A) | Espacio |
| Atrás (B) | Retroceso |
| Start | Enter |

Todo se puede cambiar en **Controles**. Con un mando no hace falta configurar
nada.

## Dónde están las cosas

- Partidas: `Documentos\forza2\profiles\<perfil>\`
- Tus ajustes: `forza2.user.toml` (lo escribe el launcher)
- Registros, si algo falla: carpeta `logs\` junto al juego
- Novedades de cada versión: `CHANGELOG.md`

## Si algo falla

Si el juego se cierra, el launcher te lo indica. Adjunta el último archivo de
`logs\` al reportar el problema.

---

# English

Run `Forza2Launcher.exe`, go to **Install** and import your Forza
Motorsport 2 disc image (`.iso`); then create a profile under **Profiles** and
press **PLAY**. A Vulkan-capable graphics card is required. To race online,
keep *Play online* on in the **Online** card (*Profiles* tab) and open
**Multiplayer → System Link** in the game. Saves live in
`Documents\forza2\profiles\<profile>\`, logs in `logs\` next to the game.
