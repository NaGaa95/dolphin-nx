// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/Localization.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/JsonUtil.h"
#include "DolphinSwitch/SystemLanguage.h"

namespace DolphinSwitch
{
namespace
{
constexpr std::array<LauncherLanguage, 7> LANGUAGES = {{{"system", "System"},
                                                        {"en", "English"},
                                                        {"fr", "Français"},
                                                        {"de", "Deutsch"},
                                                        {"es", "Español"},
                                                        {"it", "Italiano"},
                                                        {"pt", "Português"}}};

struct LauncherTranslation
{
  std::string_view source;
  std::string_view fr;
  std::string_view de;
  std::string_view es;
  std::string_view it;
  std::string_view pt;
};

// These are Switch-launcher-specific strings which are not present in Dolphin's regular gettext
// catalog yet. Everything else falls through to the catalog generated from Languages/po.
constexpr std::array LAUNCHER_TRANSLATIONS = {
    LauncherTranslation{"System", "Système", "System", "Sistema", "Sistema", "Sistema"},
    LauncherTranslation{"Language", "Langue", "Sprache", "Idioma", "Lingua", "Idioma"},
    LauncherTranslation{"Applet mode installer", "Installation en mode applet",
                        "Applet-Modus-Installer", "Instalador del modo applet",
                        "Installazione in modalità applet", "Instalador do modo applet"},
    LauncherTranslation{"Dolphin is running in applet mode.", "Dolphin fonctionne en mode applet.",
                        "Dolphin läuft im Applet-Modus.",
                        "Dolphin se está ejecutando en modo applet.",
                        "Dolphin è in esecuzione in modalità applet.",
                        "O Dolphin está em execução no modo applet."},
    LauncherTranslation{
        "Applet mode has limited memory and is not suitable for emulation.",
        "Le mode applet dispose de peu de mémoire et ne convient pas à l'émulation.",
        "Der Applet-Modus hat wenig Speicher und eignet sich nicht zur Emulation.",
        "El modo applet tiene memoria limitada y no es adecuado para emular.",
        "La modalità applet ha memoria limitata e non è adatta all'emulazione.",
        "O modo applet tem memória limitada e não é adequado para emulação."},
    LauncherTranslation{
        "Install a HOME Menu shortcut to run Dolphin with full memory and normal performance.",
        "Installez un raccourci dans le menu HOME pour utiliser toute la mémoire et les "
        "performances normales.",
        "Installiere eine HOME-Menü-Verknüpfung, damit Dolphin mit vollem Speicher und normaler "
        "Leistung läuft.",
        "Instala un acceso directo en el menú HOME para usar toda la memoria y el rendimiento "
        "normal.",
        "Installa un collegamento nel menu HOME per usare tutta la memoria e le prestazioni "
        "normali.",
        "Instale um atalho no Menu HOME para usar toda a memória e o desempenho normal."},
    LauncherTranslation{"Install Dolphin to HOME Menu", "Installer Dolphin dans le menu HOME",
                        "Dolphin im HOME-Menü installieren", "Instalar Dolphin en el menú HOME",
                        "Installa Dolphin nel menu HOME", "Instalar o Dolphin no Menu HOME"},
    LauncherTranslation{
        "Installing HOME Menu shortcut...", "Installation du raccourci du menu HOME...",
        "HOME-Menü-Verknüpfung wird installiert...",
        "Instalando el acceso directo del menú HOME...",
        "Installazione del collegamento nel menu HOME...", "A instalar o atalho do Menu HOME..."},
    LauncherTranslation{
        "Dolphin was installed on the HOME Menu.", "Dolphin a été installé dans le menu HOME.",
        "Dolphin wurde im HOME-Menü installiert.", "Dolphin se instaló en el menú HOME.",
        "Dolphin è stato installato nel menu HOME.", "O Dolphin foi instalado no Menu HOME."},
    LauncherTranslation{
        "You can close this installer and launch Dolphin from HOME.",
        "Vous pouvez fermer cet installateur et lancer Dolphin depuis le menu HOME.",
        "Du kannst diesen Installer schließen und Dolphin über das HOME-Menü starten.",
        "Puedes cerrar este instalador e iniciar Dolphin desde el menú HOME.",
        "Puoi chiudere il programma di installazione e avviare Dolphin dal menu HOME.",
        "Pode fechar este instalador e iniciar o Dolphin a partir do Menu HOME."},
    LauncherTranslation{"Installation failed", "Échec de l'installation",
                        "Installation fehlgeschlagen", "Error de instalación",
                        "Installazione non riuscita", "Falha na instalação"},
    LauncherTranslation{"Try again", "Réessayer", "Erneut versuchen", "Reintentar", "Riprova",
                        "Tentar novamente"},
    LauncherTranslation{"Install", "Installer", "Installieren", "Instalar", "Installa", "Instalar"},
    LauncherTranslation{"Exit", "Quitter", "Beenden", "Salir", "Esci", "Sair"},
    LauncherTranslation{"Reset", "Réinitialiser", "Zurücksetzen", "Restablecer", "Ripristina",
                        "Repor"},
    LauncherTranslation{"Setting reset to default", "Paramètre réinitialisé à sa valeur par défaut",
                        "Einstellung auf Standardwert zurückgesetzt",
                        "Ajuste restablecido al valor predeterminado",
                        "Impostazione ripristinata al valore predefinito",
                        "Definição reposta para o valor predefinido"},
    LauncherTranslation{"Loading game library...", "Chargement de la bibliothèque...",
                        "Spielebibliothek wird geladen...", "Cargando la biblioteca de juegos...",
                        "Caricamento della raccolta giochi...",
                        "A carregar a biblioteca de jogos..."},
    LauncherTranslation{"The first page will appear as soon as it is ready.",
                        "La première page s'affichera dès qu'elle sera prête.",
                        "Die erste Seite erscheint, sobald sie bereit ist.",
                        "La primera página aparecerá en cuanto esté lista.",
                        "La prima pagina apparirà non appena sarà pronta.",
                        "A primeira página aparecerá assim que estiver pronta."},
    LauncherTranslation{"Installed", "Installé", "Installiert", "Instalado", "Installato",
                        "Instalado"},
    LauncherTranslation{"Theme", "Thème", "Design", "Tema", "Tema", "Tema"},
    LauncherTranslation{"Games per row", "Jeux par ligne", "Spiele pro Zeile", "Juegos por fila",
                        "Giochi per riga", "Jogos por linha"},
    LauncherTranslation{"Rows per page", "Lignes par page", "Zeilen pro Seite", "Filas por página",
                        "Righe per pagina", "Linhas por página"},
    LauncherTranslation{"Show game titles", "Afficher les titres des jeux", "Spieltitel anzeigen",
                        "Mostrar títulos de juegos", "Mostra i titoli dei giochi",
                        "Mostrar títulos dos jogos"},
    LauncherTranslation{"Show region flags", "Afficher les drapeaux de région",
                        "Regionsflaggen anzeigen", "Mostrar banderas de región",
                        "Mostra bandiere regionali", "Mostrar bandeiras de região"},
    LauncherTranslation{"Show custom settings badges",
                        "Afficher les indicateurs de paramètres personnalisés",
                        "Markierungen für benutzerdefinierte Einstellungen anzeigen",
                        "Mostrar indicadores de ajustes personalizados",
                        "Mostra indicatori delle impostazioni personalizzate",
                        "Mostrar indicadores de definições personalizadas"},
    LauncherTranslation{"UI animations", "Animations de l'interface", "UI-Animationen",
                        "Animaciones de la interfaz", "Animazioni dell'interfaccia",
                        "Animações da interface"},
    LauncherTranslation{"Sound effects", "Effets sonores", "Soundeffekte", "Efectos de sonido",
                        "Effetti sonori", "Efeitos sonoros"},
    LauncherTranslation{"Check updates at boot", "Vérifier au démarrage",
                        "Beim Start nach Updates suchen", "Buscar actualizaciones al iniciar",
                        "Controlla aggiornamenti all'avvio", "Procurar atualizações ao iniciar"},
    LauncherTranslation{"Check for Updates", "Rechercher des mises à jour", "Nach Updates suchen",
                        "Buscar actualizaciones", "Controlla aggiornamenti",
                        "Procurar atualizações"},
    LauncherTranslation{"Library & storage", "Bibliothèque et stockage", "Bibliothek & Speicher",
                        "Biblioteca y almacenamiento", "Libreria e archiviazione",
                        "Biblioteca e armazenamento"},
    LauncherTranslation{"Download covers", "Télécharger les jaquettes", "Cover herunterladen",
                        "Descargar carátulas", "Scarica copertine", "Transferir capas"},
    LauncherTranslation{"Download covers?", "Télécharger les jaquettes ?", "Cover herunterladen?",
                        "¿Descargar las carátulas?", "Scaricare le copertine?",
                        "Transferir as capas?"},
    LauncherTranslation{"Downloading covers", "Téléchargement des jaquettes",
                        "Cover werden heruntergeladen", "Descargando carátulas",
                        "Download delle copertine", "A transferir capas"},
    LauncherTranslation{"Safely eject", "Éjecter en toute sécurité", "Sicher auswerfen",
                        "Expulsar de forma segura", "Espelli in sicurezza", "Ejetar com segurança"},
    LauncherTranslation{"Exit Dolphin?", "Quitter Dolphin ?", "Dolphin beenden?",
                        "¿Salir de Dolphin?", "Uscire da Dolphin?", "Sair do Dolphin?"},
    LauncherTranslation{
        "Active scans and network operations will be cancelled safely.",
        "Les analyses et opérations réseau actives seront annulées en toute sécurité.",
        "Aktive Scans und Netzwerkvorgänge werden sicher abgebrochen.",
        "Los análisis y las operaciones de red activos se cancelarán de forma segura.",
        "Le scansioni e le operazioni di rete attive verranno annullate in sicurezza.",
        "As análises e operações de rede ativas serão canceladas com segurança."},
    LauncherTranslation{"Return to the HOME Menu?", "Retourner au menu HOME ?",
                        "Zum HOME-Menü zurückkehren?", "¿Volver al menú HOME?",
                        "Tornare al menu HOME?", "Voltar ao Menu HOME?"},
    LauncherTranslation{"Closing Dolphin...", "Fermeture de Dolphin...", "Dolphin wird beendet...",
                        "Cerrando Dolphin...", "Chiusura di Dolphin...", "A fechar o Dolphin..."},
    LauncherTranslation{"Finishing background operations safely.",
                        "Finalisation sécurisée des opérations en arrière-plan.",
                        "Hintergrundvorgänge werden sicher abgeschlossen.",
                        "Finalizando de forma segura las operaciones en segundo plano.",
                        "Completamento sicuro delle operazioni in background.",
                        "A concluir as operações em segundo plano com segurança."},
    LauncherTranslation{"Frame Generation", "Génération d'images", "Frame-Generierung",
                        "Generación de fotogramas", "Generazione fotogrammi",
                        "Geração de fotogramas"},
    LauncherTranslation{"CPU / Emulation", "CPU / Émulation", "CPU / Emulation", "CPU / Emulación",
                        "CPU / Emulazione", "CPU / Emulação"},
    LauncherTranslation{"GameCube & Wii", "GameCube et Wii", "GameCube & Wii", "GameCube y Wii",
                        "GameCube e Wii", "GameCube e Wii"},
    LauncherTranslation{"Controller / Input", "Manette / Entrées", "Controller / Eingabe",
                        "Mando / Entrada", "Controller / Input", "Comando / Entrada"},
    LauncherTranslation{"Online & accounts", "En ligne et comptes", "Online & Konten",
                        "En línea y cuentas", "Online e account", "Online e contas"},
    LauncherTranslation{"Settings", "Paramètres", "Einstellungen", "Ajustes", "Impostazioni",
                        "Definições"},
    LauncherTranslation{"Choose", "Choisir", "Auswählen", "Elegir", "Scegli", "Escolher"},
    LauncherTranslation{"Change", "Modifier", "Ändern", "Cambiar", "Cambia", "Alterar"},
    LauncherTranslation{"Info", "Infos", "Info", "Info", "Info", "Info"},
    LauncherTranslation{"Back", "Retour", "Zurück", "Atrás", "Indietro", "Voltar"},
    LauncherTranslation{"On", "Activé", "Ein", "Activado", "Attivo", "Ligado"},
    LauncherTranslation{"Off", "Désactivé", "Aus", "Desactivado", "Disattivato", "Desligado"},
    LauncherTranslation{"Enabled", "Activé", "Aktiviert", "Activado", "Attivato", "Ativado"},
    LauncherTranslation{"Disabled", "Désactivé", "Deaktiviert", "Desactivado", "Disattivato",
                        "Desativado"},
    LauncherTranslation{"Global", "Global", "Global", "Global", "Globale", "Global"},
    LauncherTranslation{"Use global", "Utiliser le réglage global", "Global verwenden",
                        "Usar ajuste global", "Usa impostazione globale", "Usar definição global"},
    LauncherTranslation{"Settings group", "Groupe de paramètres", "Einstellungsgruppe",
                        "Grupo de ajustes", "Gruppo di impostazioni", "Grupo de definições"},
    LauncherTranslation{"Dolphin setting", "Paramètre Dolphin", "Dolphin-Einstellung",
                        "Ajuste de Dolphin", "Impostazione Dolphin", "Definição do Dolphin"},
    LauncherTranslation{"Setting", "Paramètre", "Einstellung", "Ajuste", "Impostazione",
                        "Definição"},
    LauncherTranslation{"Global setting", "Paramètre global", "Globale Einstellung",
                        "Ajuste global", "Impostazione globale", "Definição global"},
    LauncherTranslation{"Global settings", "Paramètres globaux", "Globale Einstellungen",
                        "Ajustes globales", "Impostazioni globali", "Definições globais"},
    LauncherTranslation{"Per-game setting", "Paramètre par jeu", "Spieleinstellung",
                        "Ajuste por juego", "Impostazione per gioco", "Definição por jogo"},
    LauncherTranslation{"Per-game settings", "Paramètres par jeu", "Spieleinstellungen",
                        "Ajustes por juego", "Impostazioni per gioco", "Definições por jogo"},
    LauncherTranslation{"Management action", "Action de gestion", "Verwaltungsaktion",
                        "Acción de gestión", "Azione di gestione", "Ação de gestão"},
    LauncherTranslation{"Setting info", "Informations du paramètre", "Einstellungsinfo",
                        "Información del ajuste", "Informazioni impostazione",
                        "Informações da definição"},
    LauncherTranslation{"Current: ", "Actuel : ", "Aktuell: ", "Actual: ", "Attuale: ", "Atual: "},
    LauncherTranslation{"Close", "Fermer", "Schließen", "Cerrar", "Chiudi", "Fechar"},
    LauncherTranslation{"Touch anywhere to close", "Touchez n'importe où pour fermer",
                        "Zum Schließen beliebig tippen", "Toca en cualquier lugar para cerrar",
                        "Tocca un punto qualsiasi per chiudere",
                        "Toque em qualquer lugar para fechar"},
    LauncherTranslation{
        "Opens this group of Dolphin settings.", "Ouvre ce groupe de paramètres Dolphin.",
        "Öffnet diese Gruppe von Dolphin-Einstellungen.", "Abre este grupo de ajustes de Dolphin.",
        "Apre questo gruppo di impostazioni di Dolphin.",
        "Abre este grupo de definições do Dolphin."},
    LauncherTranslation{
        "Opens this Dolphin management action or a file-selection screen.",
        "Ouvre cette action de gestion Dolphin ou un écran de sélection de fichier.",
        "Öffnet diese Dolphin-Verwaltungsaktion oder eine Dateiauswahl.",
        "Abre esta acción de gestión de Dolphin o una pantalla de selección de archivos.",
        "Apre questa azione di gestione di Dolphin o una schermata di selezione file.",
        "Abre esta ação de gestão do Dolphin ou um ecrã de seleção de ficheiros."},
    LauncherTranslation{"Changes this Dolphin option. Keep the default value when troubleshooting "
                        "an unexpected game-specific problem.",
                        "Modifie cette option de Dolphin. Conservez la valeur par défaut lors du "
                        "diagnostic d'un problème inattendu propre à un jeu.",
                        "Ändert diese Dolphin-Option. Behalte bei der Fehlersuche nach einem "
                        "unerwarteten spielspezifischen Problem den Standardwert bei.",
                        "Cambia esta opción de Dolphin. Mantén el valor predeterminado al "
                        "diagnosticar un problema inesperado de un juego.",
                        "Modifica questa opzione di Dolphin. Mantieni il valore predefinito "
                        "durante la diagnosi di un problema imprevisto specifico del gioco.",
                        "Altera esta opção do Dolphin. Mantém o valor predefinido ao diagnosticar "
                        "um problema inesperado específico de um jogo."},
    LauncherTranslation{"Game settings", "Paramètres du jeu", "Spieleinstellungen",
                        "Ajustes del juego", "Impostazioni di gioco", "Definições do jogo"},
    LauncherTranslation{"Game emulation settings", "Émulation du jeu",
                        "Emulationseinstellungen des Spiels", "Emulación del juego",
                        "Emulazione del gioco", "Emulação do jogo"},
    LauncherTranslation{"Game advanced CPU & timing", "CPU et synchronisation avancés du jeu",
                        "Erweiterte CPU- und Timing-Einstellungen des Spiels",
                        "CPU y temporización avanzadas del juego",
                        "CPU e temporizzazione avanzate del gioco",
                        "CPU e temporização avançadas do jogo"},
    LauncherTranslation{"Game graphics settings", "Graphismes du jeu",
                        "Grafikeinstellungen des Spiels", "Gráficos del juego", "Grafica del gioco",
                        "Gráficos do jogo"},
    LauncherTranslation{"Game frame generation", "Génération d'images du jeu",
                        "Frame-Generierung des Spiels", "Generación de fotogramas del juego",
                        "Generazione fotogrammi del gioco", "Geração de fotogramas do jogo"},
    LauncherTranslation{"Game graphics enhancements", "Améliorations graphiques du jeu",
                        "Grafikverbesserungen des Spiels", "Mejoras gráficas del juego",
                        "Miglioramenti grafici del gioco", "Melhorias gráficas do jogo"},
    LauncherTranslation{"Game graphics hacks", "Hacks graphiques du jeu", "Grafik-Hacks des Spiels",
                        "Hacks gráficos del juego", "Hack grafici del gioco",
                        "Hacks gráficos do jogo"},
    LauncherTranslation{"Game audio", "Audio du jeu", "Audio des Spiels", "Audio del juego",
                        "Audio del gioco", "Áudio do jogo"},
    LauncherTranslation{"Game console settings", "Paramètres de console du jeu",
                        "Konsoleneinstellungen des Spiels", "Ajustes de consola del juego",
                        "Impostazioni console del gioco", "Definições de consola do jogo"},
    LauncherTranslation{"Game controllers", "Manettes du jeu", "Controller des Spiels",
                        "Mandos del juego", "Controller del gioco", "Comandos do jogo"},
    LauncherTranslation{"Graphics", "Graphismes", "Grafik", "Gráficos", "Grafica", "Gráficos"},
    LauncherTranslation{"Audio", "Audio", "Audio", "Audio", "Audio", "Áudio"},
    LauncherTranslation{"Patches / AR / Gecko / Riivolution",
                        "Correctifs / AR / Gecko / Riivolution",
                        "Patches / AR / Gecko / Riivolution", "Parches / AR / Gecko / Riivolution",
                        "Patch / AR / Gecko / Riivolution", "Patches / AR / Gecko / Riivolution"},
    LauncherTranslation{"Patches, cheats & Riivolution", "Correctifs, codes et Riivolution",
                        "Patches, Cheats & Riivolution", "Parches, trucos y Riivolution",
                        "Patch, trucchi e Riivolution", "Patches, batotas e Riivolution"},
    LauncherTranslation{"Launch", "Lancer", "Starten", "Iniciar", "Avvia", "Iniciar"},
    LauncherTranslation{"Rename game", "Renommer le jeu", "Spiel umbenennen",
                        "Cambiar nombre del juego", "Rinomina gioco", "Mudar nome do jogo"},
    LauncherTranslation{"Change cover (SteamGridDB)", "Changer la jaquette (SteamGridDB)",
                        "Cover ändern (SteamGridDB)", "Cambiar carátula (SteamGridDB)",
                        "Cambia copertina (SteamGridDB)", "Alterar capa (SteamGridDB)"},
    LauncherTranslation{"Download cover (SteamGridDB)", "Télécharger la jaquette (SteamGridDB)",
                        "Cover herunterladen (SteamGridDB)", "Descargar carátula (SteamGridDB)",
                        "Scarica copertina (SteamGridDB)", "Transferir capa (SteamGridDB)"},
    LauncherTranslation{"Cover settings", "Paramètres de la jaquette", "Cover-Einstellungen",
                        "Ajustes de la carátula", "Impostazioni copertina",
                        "Definições da capa"},
    LauncherTranslation{"Download from SteamGridDB", "Télécharger depuis SteamGridDB",
                        "Von SteamGridDB herunterladen", "Descargar desde SteamGridDB",
                        "Scarica da SteamGridDB", "Transferir do SteamGridDB"},
    LauncherTranslation{"Import cover from file", "Importer une jaquette depuis un fichier",
                        "Cover aus Datei importieren", "Importar carátula desde un archivo",
                        "Importa copertina da file", "Importar capa de um ficheiro"},
    LauncherTranslation{"Remove custom cover", "Supprimer la jaquette personnalisée",
                        "Benutzerdefiniertes Cover entfernen",
                        "Eliminar carátula personalizada", "Rimuovi copertina personalizzata",
                        "Remover capa personalizada"},
    LauncherTranslation{"Online artwork", "Illustration en ligne", "Online-Cover",
                        "Carátula en línea", "Copertina online", "Capa online"},
    LauncherTranslation{"Local image", "Image locale", "Lokales Bild", "Imagen local",
                        "Immagine locale", "Imagem local"},
    LauncherTranslation{"Restore game artwork", "Restaurer l'illustration du jeu",
                        "Spielgrafik wiederherstellen", "Restaurar ilustración del juego",
                        "Ripristina grafica del gioco", "Restaurar imagem do jogo"},
    LauncherTranslation{"Select local cover", "Sélectionner une jaquette locale",
                        "Lokales Cover auswählen", "Seleccionar una carátula local",
                        "Seleziona copertina locale", "Selecionar uma capa local"},
    LauncherTranslation{"Importing local cover", "Importation de la jaquette locale",
                        "Lokales Cover wird importiert", "Importando la carátula local",
                        "Importazione della copertina locale", "A importar a capa local"},
    LauncherTranslation{"Cover imported", "Jaquette importée", "Cover importiert",
                        "Carátula importada", "Copertina importata", "Capa importada"},
    LauncherTranslation{"Cover import failed", "Échec de l'importation de la jaquette",
                        "Cover-Import fehlgeschlagen", "Error al importar la carátula",
                        "Importazione della copertina non riuscita", "Falha ao importar a capa"},
    LauncherTranslation{"Cover removal failed", "Échec de la suppression de la jaquette",
                        "Cover konnte nicht entfernt werden",
                        "Error al eliminar la carátula",
                        "Rimozione della copertina non riuscita", "Falha ao remover a capa"},
    LauncherTranslation{"Custom cover removed", "Jaquette personnalisée supprimée",
                        "Benutzerdefiniertes Cover entfernt",
                        "Carátula personalizada eliminada", "Copertina personalizzata rimossa",
                        "Capa personalizada removida"},
    LauncherTranslation{"Remove custom cover?", "Supprimer la jaquette personnalisée ?",
                        "Benutzerdefiniertes Cover entfernen?",
                        "¿Eliminar la carátula personalizada?",
                        "Rimuovere la copertina personalizzata?",
                        "Remover a capa personalizada?"},
    LauncherTranslation{
        "The downloaded or imported cover will be deleted.",
        "La jaquette téléchargée ou importée sera supprimée.",
        "Das heruntergeladene oder importierte Cover wird gelöscht.",
        "La carátula descargada o importada se eliminará.",
        "La copertina scaricata o importata verrà eliminata.",
        "A capa transferida ou importada será eliminada."},
    LauncherTranslation{
        "Dolphin will use the game's embedded artwork when available.",
        "Dolphin utilisera l'illustration intégrée au jeu lorsqu'elle est disponible.",
        "Dolphin verwendet die im Spiel eingebettete Grafik, falls verfügbar.",
        "Dolphin usará la ilustración integrada del juego cuando esté disponible.",
        "Dolphin userà la grafica integrata nel gioco quando disponibile.",
        "O Dolphin usará a imagem incorporada no jogo quando estiver disponível."},
    LauncherTranslation{"Local artwork", "Illustration locale", "Lokale Grafik",
                        "Ilustración local", "Grafica locale", "Imagem local"},
    LauncherTranslation{"Artwork management", "Gestion des illustrations",
                        "Cover-Verwaltung", "Gestión de ilustraciones", "Gestione copertine",
                        "Gestão de capas"},
    LauncherTranslation{
        "Searches SteamGridDB for this game and replaces its custom cover with the selected "
        "online artwork.",
        "Recherche ce jeu sur SteamGridDB et remplace sa jaquette personnalisée par "
        "l'illustration en ligne sélectionnée.",
        "Sucht dieses Spiel auf SteamGridDB und ersetzt sein benutzerdefiniertes Cover durch die "
        "ausgewählte Online-Grafik.",
        "Busca este juego en SteamGridDB y sustituye su carátula personalizada por la ilustración "
        "en línea seleccionada.",
        "Cerca questo gioco su SteamGridDB e sostituisce la copertina personalizzata con la "
        "grafica online selezionata.",
        "Procura este jogo no SteamGridDB e substitui a capa personalizada pela imagem online "
        "selecionada."},
    LauncherTranslation{
        "Imports a PNG, JPEG, WebP or BMP image from SD, USB or SMB storage and stores it as this "
        "game's custom cover.",
        "Importe une image PNG, JPEG, WebP ou BMP depuis un stockage SD, USB ou SMB et l'utilise "
        "comme jaquette personnalisée de ce jeu.",
        "Importiert ein PNG-, JPEG-, WebP- oder BMP-Bild von einem SD-, USB- oder SMB-Speicher "
        "und speichert es als benutzerdefiniertes Cover dieses Spiels.",
        "Importa una imagen PNG, JPEG, WebP o BMP desde un almacenamiento SD, USB o SMB y la "
        "guarda como carátula personalizada de este juego.",
        "Importa un'immagine PNG, JPEG, WebP o BMP da un archivio SD, USB o SMB e la salva come "
        "copertina personalizzata del gioco.",
        "Importa uma imagem PNG, JPEG, WebP ou BMP de um armazenamento SD, USB ou SMB e guarda-a "
        "como capa personalizada deste jogo."},
    LauncherTranslation{
        "Deletes this game's custom cover. Dolphin falls back to embedded game artwork when "
        "available.",
        "Supprime la jaquette personnalisée de ce jeu. Dolphin réutilise l'illustration intégrée "
        "au jeu lorsqu'elle est disponible.",
        "Löscht das benutzerdefinierte Cover dieses Spiels. Dolphin verwendet anschließend die "
        "eingebettete Spielgrafik, falls verfügbar.",
        "Elimina la carátula personalizada de este juego. Dolphin vuelve a usar la ilustración "
        "integrada cuando está disponible.",
        "Elimina la copertina personalizzata del gioco. Dolphin torna alla grafica integrata, se "
        "disponibile.",
        "Elimina a capa personalizada deste jogo. O Dolphin volta a usar a imagem incorporada, "
        "quando disponível."},
    LauncherTranslation{"The selected cover file is unavailable.",
                        "Le fichier de jaquette sélectionné est indisponible.",
                        "Die ausgewählte Cover-Datei ist nicht verfügbar.",
                        "El archivo de carátula seleccionado no está disponible.",
                        "Il file della copertina selezionato non è disponibile.",
                        "O ficheiro de capa selecionado não está disponível."},
    LauncherTranslation{"The selected cover file is too large.",
                        "Le fichier de jaquette sélectionné est trop volumineux.",
                        "Die ausgewählte Cover-Datei ist zu groß.",
                        "El archivo de carátula seleccionado es demasiado grande.",
                        "Il file della copertina selezionato è troppo grande.",
                        "O ficheiro de capa selecionado é demasiado grande."},
    LauncherTranslation{"Dolphin could not prepare the cover file safely.",
                        "Dolphin n'a pas pu préparer le fichier de jaquette en toute sécurité.",
                        "Dolphin konnte die Cover-Datei nicht sicher vorbereiten.",
                        "Dolphin no pudo preparar el archivo de carátula de forma segura.",
                        "Dolphin non ha potuto preparare il file della copertina in sicurezza.",
                        "O Dolphin não conseguiu preparar o ficheiro de capa em segurança."},
    LauncherTranslation{"The selected file is not a supported image.",
                        "Le fichier sélectionné n'est pas une image prise en charge.",
                        "Die ausgewählte Datei ist kein unterstütztes Bild.",
                        "El archivo seleccionado no es una imagen compatible.",
                        "Il file selezionato non è un'immagine supportata.",
                        "O ficheiro selecionado não é uma imagem suportada."},
    LauncherTranslation{"The selected image dimensions are too large.",
                        "Les dimensions de l'image sélectionnée sont trop grandes.",
                        "Die Abmessungen des ausgewählten Bildes sind zu groß.",
                        "Las dimensiones de la imagen seleccionada son demasiado grandes.",
                        "Le dimensioni dell'immagine selezionata sono troppo grandi.",
                        "As dimensões da imagem selecionada são demasiado grandes."},
    LauncherTranslation{"Dolphin could not convert the selected image to PNG.",
                        "Dolphin n'a pas pu convertir l'image sélectionnée en PNG.",
                        "Dolphin konnte das ausgewählte Bild nicht in PNG umwandeln.",
                        "Dolphin no pudo convertir la imagen seleccionada a PNG.",
                        "Dolphin non ha potuto convertire l'immagine selezionata in PNG.",
                        "O Dolphin não conseguiu converter a imagem selecionada para PNG."},
    LauncherTranslation{"Dolphin could not verify the converted cover.",
                        "Dolphin n'a pas pu vérifier la jaquette convertie.",
                        "Dolphin konnte das konvertierte Cover nicht überprüfen.",
                        "Dolphin no pudo verificar la carátula convertida.",
                        "Dolphin non ha potuto verificare la copertina convertita.",
                        "O Dolphin não conseguiu verificar a capa convertida."},
    LauncherTranslation{"Dolphin could not save the converted cover.",
                        "Dolphin n'a pas pu enregistrer la jaquette convertie.",
                        "Dolphin konnte das konvertierte Cover nicht speichern.",
                        "Dolphin no pudo guardar la carátula convertida.",
                        "Dolphin non ha potuto salvare la copertina convertita.",
                        "O Dolphin não conseguiu guardar a capa convertida."},
    LauncherTranslation{"Dolphin could not replace the current cover safely.",
                        "Dolphin n'a pas pu remplacer la jaquette actuelle en toute sécurité.",
                        "Dolphin konnte das aktuelle Cover nicht sicher ersetzen.",
                        "Dolphin no pudo sustituir la carátula actual de forma segura.",
                        "Dolphin non ha potuto sostituire la copertina attuale in sicurezza.",
                        "O Dolphin não conseguiu substituir a capa atual em segurança."},
    LauncherTranslation{"The selected cover could not be imported safely.",
                        "La jaquette sélectionnée n'a pas pu être importée en toute sécurité.",
                        "Das ausgewählte Cover konnte nicht sicher importiert werden.",
                        "La carátula seleccionada no pudo importarse de forma segura.",
                        "La copertina selezionata non ha potuto essere importata in sicurezza.",
                        "Não foi possível importar a capa selecionada em segurança."},
    LauncherTranslation{"Create HOME shortcut", "Créer un raccourci HOME",
                        "HOME-Verknüpfung erstellen", "Crear acceso directo en HOME",
                        "Crea collegamento HOME", "Criar atalho no HOME"},
    LauncherTranslation{"Icon", "Icône", "Symbol", "Icono", "Icona", "Ícone"},
    LauncherTranslation{"Name", "Nom", "Name", "Nombre", "Nome", "Nome"},
    LauncherTranslation{"Author / Version", "Auteur / Version", "Autor / Version",
                        "Autor / Versión", "Autore / Versione", "Autor / Versão"},
    LauncherTranslation{"Create shortcut", "Créer le raccourci", "Verknüpfung erstellen",
                        "Crear acceso directo", "Crea collegamento", "Criar atalho"},
    LauncherTranslation{"Pick an icon first", "Choisissez d'abord une icône",
                        "Wähle zuerst ein Symbol", "Elige primero un icono",
                        "Scegli prima un'icona", "Escolha primeiro um ícone"},
    LauncherTranslation{"Shortcut name", "Nom du raccourci", "Name der Verknüpfung",
                        "Nombre del acceso directo", "Nome del collegamento", "Nome do atalho"},
    LauncherTranslation{"Creating HOME shortcut", "Création du raccourci HOME",
                        "HOME-Verknüpfung wird erstellt", "Creando acceso directo en HOME",
                        "Creazione del collegamento HOME", "A criar atalho no HOME"},
    LauncherTranslation{"HOME shortcut installed", "Raccourci HOME installé",
                        "HOME-Verknüpfung installiert", "Acceso directo en HOME instalado",
                        "Collegamento HOME installato", "Atalho no HOME instalado"},
    LauncherTranslation{"Shortcut failed", "Échec du raccourci", "Verknüpfung fehlgeschlagen",
                        "Error al crear el acceso directo",
                        "Creazione del collegamento non riuscita", "Falha ao criar o atalho"},
    LauncherTranslation{"Manage installed content", "Gérer le contenu installé",
                        "Installierte Inhalte verwalten", "Gestionar contenido instalado",
                        "Gestisci contenuti installati", "Gerir conteúdo instalado"},
    LauncherTranslation{"Clear shader caches", "Effacer les caches de shaders",
                        "Shader-Caches leeren", "Borrar cachés de sombreadores",
                        "Svuota cache shader", "Limpar caches de shaders"},
    LauncherTranslation{"Clear game settings", "Effacer les paramètres du jeu",
                        "Spieleinstellungen löschen", "Borrar ajustes del juego",
                        "Cancella impostazioni di gioco", "Limpar definições do jogo"},
    LauncherTranslation{
        "Uninstall WAD (keep save)", "Désinstaller le WAD (garder la sauvegarde)",
        "WAD deinstallieren (Spielstand behalten)", "Desinstalar WAD (conservar partida)",
        "Disinstalla WAD (mantieni salvataggio)", "Desinstalar WAD (manter gravação)"},
    LauncherTranslation{
        "Delete game (remove from storage)", "Supprimer le jeu (retirer du stockage)",
        "Spiel löschen (vom Speicher entfernen)", "Eliminar juego (quitar del almacenamiento)",
        "Elimina gioco (rimuovi dalla memoria)", "Eliminar jogo (remover do armazenamento)"},
    LauncherTranslation{"NO COVER", "AUCUNE JAQUETTE", "KEIN COVER", "SIN CARÁTULA",
                        "NESSUNA COPERTINA", "SEM CAPA"},
    LauncherTranslation{"Game ID unavailable", "ID du jeu indisponible", "Spiel-ID nicht verfügbar",
                        "ID del juego no disponible", "ID gioco non disponibile",
                        "ID do jogo indisponível"},
    LauncherTranslation{"Unknown installation error", "Erreur d'installation inconnue",
                        "Unbekannter Installationsfehler", "Error de instalación desconocido",
                        "Errore di installazione sconosciuto", "Erro de instalação desconhecido"},
    LauncherTranslation{
        "Accurate CPU write-back cache", "Cache d'écriture différée CPU précis",
        "Präziser CPU-Rückschreibcache", "Caché de escritura diferida de CPU precisa",
        "Cache write-back CPU accurata", "Cache de escrita diferida da CPU precisa"},
    LauncherTranslation{"Active profile", "Profil actif", "Aktives Profil", "Perfil activo",
                        "Profilo attivo", "Perfil ativo"},
    LauncherTranslation{"Advanced CPU & timing", "CPU et synchronisation avancés",
                        "Erweiterte CPU- und Timing-Einstellungen", "CPU y temporización avanzadas",
                        "CPU e temporizzazione avanzate", "CPU e temporização avançadas"},
    LauncherTranslation{"Audio buffer size", "Taille du tampon audio", "Audiopuffergröße",
                        "Tamaño del búfer de audio", "Dimensione buffer audio",
                        "Tamanho do buffer de áudio"},
    LauncherTranslation{"Audio latency", "Latence audio", "Audiolatenz", "Latencia de audio",
                        "Latenza audio", "Latência de áudio"},
    LauncherTranslation{"Auto-calibration", "Étalonnage automatique", "Automatische Kalibrierung",
                        "Calibración automática", "Calibrazione automatica",
                        "Calibração automática"},
    LauncherTranslation{
        "Automatically sync SD folder", "Synchroniser automatiquement le dossier SD",
        "SD-Ordner automatisch synchronisieren", "Sincronizar automáticamente la carpeta SD",
        "Sincronizza automaticamente la cartella SD", "Sincronizar automaticamente a pasta SD"},
    LauncherTranslation{"C-Stick dead zone", "Zone morte du stick C", "C-Stick-Totzone",
                        "Zona muerta del stick C", "Zona morta C-Stick", "Zona morta do C-Stick"},
    LauncherTranslation{"Calibration guide", "Guide d'étalonnage", "Kalibrierungsanleitung",
                        "Guía de calibración", "Guida alla calibrazione", "Guia de calibração"},
    LauncherTranslation{"Cheat engine", "Moteur de triche", "Cheat-Engine", "Motor de trucos",
                        "Motore trucchi", "Motor de batotas"},
    LauncherTranslation{"Control Stick dead zone", "Zone morte du stick principal",
                        "Control-Stick-Totzone", "Zona muerta del stick de control",
                        "Zona morta stick di controllo", "Zona morta do stick de controlo"},
    LauncherTranslation{"CPU clock percentage", "Pourcentage de fréquence CPU",
                        "CPU-Takt in Prozent", "Porcentaje de reloj de CPU",
                        "Percentuale clock CPU", "Percentagem do relógio da CPU"},
    LauncherTranslation{"CPU engine", "Moteur CPU", "CPU-Engine", "Motor de CPU", "Motore CPU",
                        "Motor da CPU"},
    LauncherTranslation{"Crop to aspect ratio", "Rogner selon le format d'image",
                        "Auf Seitenverhältnis zuschneiden", "Recortar a la relación de aspecto",
                        "Ritaglia in base alle proporzioni", "Recortar para a proporção"},
    LauncherTranslation{"Custom display gamma", "Gamma d'affichage personnalisé",
                        "Benutzerdefiniertes Display-Gamma", "Gamma de pantalla personalizado",
                        "Gamma schermo personalizzata", "Gama de ecrã personalizada"},
    LauncherTranslation{
        "Deferred EFB-access invalidation", "Invalidation différée des accès EFB",
        "Verzögerte EFB-Zugriffsinvalidierung", "Invalidación diferida del acceso EFB",
        "Invalidazione differita accesso EFB", "Invalidação diferida do acesso EFB"},
    LauncherTranslation{"Display gamma", "Gamma d'affichage", "Display-Gamma", "Gamma de pantalla",
                        "Gamma schermo", "Gama do ecrã"},
    LauncherTranslation{"Download Gecko codes", "Télécharger les codes Gecko",
                        "Gecko-Codes herunterladen", "Descargar códigos Gecko",
                        "Scarica codici Gecko", "Transferir códigos Gecko"},
    LauncherTranslation{"DSP emulation", "Émulation DSP", "DSP-Emulation", "Emulación DSP",
                        "Emulazione DSP", "Emulação DSP"},
    LauncherTranslation{"Dual Core", "Dual Core", "Dual Core", "Dual Core", "Dual Core",
                        "Dual Core"},
    LauncherTranslation{"Emulated CPU clock override", "Remplacement de fréquence du CPU émulé",
                        "Emulierten CPU-Takt überschreiben", "Anulación del reloj de CPU emulada",
                        "Override clock CPU emulata", "Substituição do relógio da CPU emulada"},
    LauncherTranslation{"Emulated device", "Périphérique émulé", "Emuliertes Gerät",
                        "Dispositivo emulado", "Dispositivo emulato", "Dispositivo emulado"},
    LauncherTranslation{"Enable Wii Remote rumble", "Activer les vibrations de la télécommande Wii",
                        "Wii-Fernbedienungs-Vibration aktivieren",
                        "Activar vibración del mando de Wii", "Abilita vibrazione telecomando Wii",
                        "Ativar vibração do Comando Wii"},
    LauncherTranslation{
        "Enable Wii Remote speaker", "Activer le haut-parleur de la télécommande Wii",
        "Wii-Fernbedienungslautsprecher aktivieren", "Activar altavoz del mando de Wii",
        "Abilita altoparlante telecomando Wii", "Ativar altifalante do Comando Wii"},
    LauncherTranslation{"Extension mapping", "Mappage de l'extension", "Erweiterungsbelegung",
                        "Asignación de extensión", "Mappatura estensione",
                        "Mapeamento da extensão"},
    LauncherTranslation{"Extension stick dead zone", "Zone morte du stick de l'extension",
                        "Totzone des Erweiterungssticks", "Zona muerta del stick de extensión",
                        "Zona morta stick estensione", "Zona morta do stick da extensão"},
    LauncherTranslation{"Fast disc speed", "Vitesse de disque rapide",
                        "Schnelle Laufwerksgeschwindigkeit", "Velocidad de disco rápida",
                        "Velocità disco rapida", "Velocidade rápida do disco"},
    LauncherTranslation{"Flow resolution", "Résolution du flux", "Flow-Auflösung",
                        "Resolución de flujo", "Risoluzione flusso", "Resolução do fluxo"},
    LauncherTranslation{"Full resolution per eye", "Résolution complète par œil",
                        "Volle Auflösung pro Auge", "Resolución completa por ojo",
                        "Risoluzione completa per occhio", "Resolução total por olho"},
    LauncherTranslation{"Game color space", "Espace colorimétrique du jeu", "Farbraum des Spiels",
                        "Espacio de color del juego", "Spazio colore del gioco",
                        "Espaço de cor do jogo"},
    LauncherTranslation{"GameCube language", "Langue GameCube", "GameCube-Sprache",
                        "Idioma de GameCube", "Lingua GameCube", "Idioma da GameCube"},
    LauncherTranslation{"GameCube Slot A / B", "Slot GameCube A / B", "GameCube-Steckplatz A / B",
                        "Ranura GameCube A / B", "Slot GameCube A / B", "Ranhura GameCube A / B"},
    LauncherTranslation{"GBA cartridge path", "Chemin de la cartouche GBA", "Pfad zum GBA-Modul",
                        "Ruta del cartucho GBA", "Percorso cartuccia GBA",
                        "Caminho do cartucho GBA"},
    LauncherTranslation{"GCI folder path", "Chemin du dossier GCI", "Pfad zum GCI-Ordner",
                        "Ruta de la carpeta GCI", "Percorso cartella GCI", "Caminho da pasta GCI"},
    LauncherTranslation{"Gyro dead zone", "Zone morte du gyroscope", "Gyro-Totzone",
                        "Zona muerta del giroscopio", "Zona morta giroscopio",
                        "Zona morta do giroscópio"},
    LauncherTranslation{"Gyro pointer", "Pointeur gyroscopique", "Gyro-Zeiger",
                        "Puntero giroscópico", "Puntatore giroscopico", "Ponteiro giroscópico"},
    LauncherTranslation{"HDR paper white", "Blanc de référence HDR", "HDR-Papierweiß",
                        "Blanco de papel HDR", "Bianco carta HDR", "Branco de papel HDR"},
    LauncherTranslation{"Ignore EFB format changes", "Ignorer les changements de format EFB",
                        "EFB-Formatänderungen ignorieren", "Ignorar cambios de formato EFB",
                        "Ignora modifiche formato EFB", "Ignorar alterações de formato EFB"},
    LauncherTranslation{"Interactive mapping", "Mappage interactif", "Interaktive Belegung",
                        "Asignación interactiva", "Mappatura interattiva", "Mapeamento interativo"},
    LauncherTranslation{"IR relative input", "Entrée IR relative", "Relative IR-Eingabe",
                        "Entrada IR relativa", "Input IR relativo", "Entrada IR relativa"},
    LauncherTranslation{"IR sensitivity", "Sensibilité IR", "IR-Empfindlichkeit", "Sensibilidad IR",
                        "Sensibilità IR", "Sensibilidade IR"},
    LauncherTranslation{"IR total pitch", "Inclinaison IR totale", "Gesamte IR-Neigung",
                        "Inclinación total IR", "Pitch totale IR", "Inclinação total IR"},
    LauncherTranslation{"IR total yaw", "Lacet IR total", "Gesamter IR-Gierwinkel",
                        "Guiñada total IR", "Imbardata totale IR", "Guinada total IR"},
    LauncherTranslation{"Joy-Con layout", "Disposition des Joy-Con", "Joy-Con-Layout",
                        "Disposición de los Joy-Con", "Layout Joy-Con", "Disposição dos Joy-Con"},
    LauncherTranslation{"Launch with Riivolution XML", "Lancer avec un XML Riivolution",
                        "Mit Riivolution-XML starten", "Iniciar con XML de Riivolution",
                        "Avvia con XML Riivolution", "Iniciar com XML do Riivolution"},
    LauncherTranslation{"Launcher", "Launcher", "Launcher", "Launcher", "Launcher", "Launcher"},
    LauncherTranslation{"Load profile", "Charger le profil", "Profil laden", "Cargar perfil",
                        "Carica profilo", "Carregar perfil"},
    LauncherTranslation{"Lossless.dll", "Lossless.dll", "Lossless.dll", "Lossless.dll",
                        "Lossless.dll", "Lossless.dll"},
    LauncherTranslation{"LSFG 2x (Vulkan only)", "LSFG 2x (Vulkan uniquement)",
                        "LSFG 2x (nur Vulkan)", "LSFG 2x (solo Vulkan)", "LSFG 2x (solo Vulkan)",
                        "LSFG 2x (apenas Vulkan)"},
    LauncherTranslation{"Memory card path", "Chemin de la carte mémoire", "Speicherkartenpfad",
                        "Ruta de la tarjeta de memoria", "Percorso scheda di memoria",
                        "Caminho do cartão de memória"},
    LauncherTranslation{"MMU emulation", "Émulation MMU", "MMU-Emulation", "Emulación MMU",
                        "Emulazione MMU", "Emulação MMU"},
    LauncherTranslation{"Motion & pointer", "Mouvement et pointeur", "Bewegung & Zeiger",
                        "Movimiento y puntero", "Movimento e puntatore", "Movimento e ponteiro"},
    LauncherTranslation{"PAL60", "PAL60", "PAL60", "PAL60", "PAL60", "PAL60"},
    LauncherTranslation{"Performance mode", "Mode performance", "Leistungsmodus",
                        "Modo de rendimiento", "Modalità prestazioni", "Modo de desempenho"},
    LauncherTranslation{"Pointer recenter", "Recentrer le pointeur", "Zeiger neu zentrieren",
                        "Recentrar puntero", "Ricentra puntatore", "Recentrar ponteiro"},
    LauncherTranslation{"Pointer sensitivity", "Sensibilité du pointeur", "Zeigerempfindlichkeit",
                        "Sensibilidad del puntero", "Sensibilità puntatore",
                        "Sensibilidade do ponteiro"},
    LauncherTranslation{"Preserve pitch", "Conserver la hauteur", "Tonhöhe beibehalten",
                        "Conservar tono", "Mantieni tonalità", "Preservar tom"},
    LauncherTranslation{"Profiles", "Profils", "Profile", "Perfiles", "Profili", "Perfis"},
    LauncherTranslation{"Progressive scan", "Balayage progressif", "Progressive Abtastung",
                        "Escaneo progresivo", "Scansione progressiva", "Varrimento progressivo"},
    LauncherTranslation{"Reset game mapping", "Réinitialiser le mappage du jeu",
                        "Spielbelegung zurücksetzen", "Restablecer asignación del juego",
                        "Ripristina mappatura del gioco", "Repor mapeamento do jogo"},
    LauncherTranslation{"Reset Switch defaults", "Réinitialiser les valeurs Switch par défaut",
                        "Switch-Standardwerte wiederherstellen", "Restablecer valores de Switch",
                        "Ripristina valori predefiniti Switch", "Repor predefinições da Switch"},
    LauncherTranslation{"RetroAchievements", "RetroAchievements", "RetroAchievements",
                        "RetroAchievements", "RetroAchievements", "RetroAchievements"},
    LauncherTranslation{
        "Right-stick pointer dead zone", "Zone morte du pointeur au stick droit",
        "Totzone des rechten Zeiger-Sticks", "Zona muerta del puntero con stick derecho",
        "Zona morta puntatore stick destro", "Zona morta do ponteiro no stick direito"},
    LauncherTranslation{"Rumble strength", "Intensité des vibrations", "Vibrationsstärke",
                        "Intensidad de vibración", "Intensità vibrazione",
                        "Intensidade da vibração"},
    LauncherTranslation{"Save as profile", "Enregistrer comme profil", "Als Profil speichern",
                        "Guardar como perfil", "Salva come profilo", "Guardar como perfil"},
    LauncherTranslation{"SD card file size", "Taille du fichier de carte SD",
                        "Dateigröße der SD-Karte", "Tamaño del archivo de tarjeta SD",
                        "Dimensione file scheda SD", "Tamanho do ficheiro do cartão SD"},
    LauncherTranslation{"SD card image path", "Chemin de l'image de carte SD",
                        "Pfad zum SD-Kartenabbild", "Ruta de la imagen de tarjeta SD",
                        "Percorso immagine scheda SD", "Caminho da imagem do cartão SD"},
    LauncherTranslation{"SD sync folder", "Dossier de synchronisation SD",
                        "SD-Synchronisierungsordner", "Carpeta de sincronización SD",
                        "Cartella sincronizzazione SD", "Pasta de sincronização SD"},
    LauncherTranslation{"Sensor bar position", "Position de la barre de capteurs",
                        "Position der Sensorleiste", "Posición de la barra de sensores",
                        "Posizione barra sensore", "Posição da barra de sensores"},
    LauncherTranslation{"Skip GameCube Main Menu", "Ignorer le menu principal GameCube",
                        "GameCube-Hauptmenü überspringen", "Omitir menú principal de GameCube",
                        "Salta menu principale GameCube", "Ignorar menu principal da GameCube"},
    LauncherTranslation{"Sound mode", "Mode audio", "Sound-Modus", "Modo de sonido",
                        "Modalità audio", "Modo de som"},
    LauncherTranslation{"SteamGridDB API key", "Clé API SteamGridDB", "SteamGridDB-API-Schlüssel",
                        "Clave API de SteamGridDB", "Chiave API SteamGridDB",
                        "Chave API do SteamGridDB"},
    LauncherTranslation{"Stereoscopic convergence", "Convergence stéréoscopique",
                        "Stereoskopische Konvergenz", "Convergencia estereoscópica",
                        "Convergenza stereoscopica", "Convergência estereoscópica"},
    LauncherTranslation{"Stereoscopic depth", "Profondeur stéréoscopique", "Stereoskopische Tiefe",
                        "Profundidad estereoscópica", "Profondità stereoscopica",
                        "Profundidade estereoscópica"},
    LauncherTranslation{"Swap stereo eyes", "Inverser les yeux stéréo", "Stereo-Augen vertauschen",
                        "Intercambiar ojos estéreo", "Scambia occhi stereo",
                        "Trocar olhos estéreo"},
    LauncherTranslation{"Switch player", "Joueur Switch", "Switch-Spieler", "Jugador de Switch",
                        "Giocatore Switch", "Jogador da Switch"},
    LauncherTranslation{"Touchscreen Wii pointer", "Pointeur Wii tactile", "Wii-Touchscreen-Zeiger",
                        "Puntero Wii táctil", "Puntatore Wii touchscreen",
                        "Ponteiro Wii no ecrã tátil"},
    LauncherTranslation{"Trigger dead zone", "Zone morte des gâchettes", "Trigger-Totzone",
                        "Zona muerta de gatillos", "Zona morta grilletti",
                        "Zona morta dos gatilhos"},
    LauncherTranslation{"Use global mapping", "Utiliser le mappage global",
                        "Globale Belegung verwenden", "Usar asignación global",
                        "Usa mappatura globale", "Usar mapeamento global"},
    LauncherTranslation{"VBI frequency percentage", "Pourcentage de fréquence VBI",
                        "VBI-Frequenz in Prozent", "Porcentaje de frecuencia VBI",
                        "Percentuale frequenza VBI", "Percentagem da frequência VBI"},
    LauncherTranslation{"Video backend", "Backend vidéo", "Grafik-Backend", "Backend de vídeo",
                        "Backend video", "Backend de vídeo"},
    LauncherTranslation{"VSync", "VSync", "VSync", "VSync", "VSync", "VSync"},
    LauncherTranslation{
        "Wait for shaders before starting", "Attendre les shaders avant le démarrage",
        "Vor dem Start auf Shader warten", "Esperar a los sombreadores antes de iniciar",
        "Attendi gli shader prima dell'avvio", "Aguardar pelos shaders antes de iniciar"},
    LauncherTranslation{"Wii language", "Langue Wii", "Wii-Sprache", "Idioma de Wii", "Lingua Wii",
                        "Idioma da Wii"},
    LauncherTranslation{"Wii Remote mapping", "Mappage de la télécommande Wii",
                        "Wii-Fernbedienungsbelegung", "Asignación del mando de Wii",
                        "Mappatura telecomando Wii", "Mapeamento do Comando Wii"},
    LauncherTranslation{
        "Wii Remote speaker volume", "Volume du haut-parleur de la télécommande Wii",
        "Lautstärke des Wii-Fernbedienungslautsprechers", "Volumen del altavoz del mando de Wii",
        "Volume altoparlante telecomando Wii", "Volume do altifalante do Comando Wii"},
    LauncherTranslation{"Wii system settings", "Paramètres système Wii", "Wii-Systemeinstellungen",
                        "Ajustes del sistema Wii", "Impostazioni di sistema Wii",
                        "Definições do sistema Wii"},
    LauncherTranslation{"Wii widescreen", "Wii en écran large", "Wii-Breitbild",
                        "Pantalla panorámica de Wii", "Schermo panoramico Wii",
                        "Ecrã panorâmico da Wii"},
};

std::string NormalizeCode(std::string_view code)
{
  std::string normalized(code);
  std::ranges::transform(normalized, normalized.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  std::ranges::replace(normalized, '_', '-');
  return normalized;
}

std::string ResolveSupportedCode(std::string_view code)
{
  const std::string normalized = NormalizeCode(code);
  for (const LauncherLanguage& language : LANGUAGES)
  {
    if (language.code != "system" && normalized == NormalizeCode(language.code))
      return std::string(language.code);
  }
  for (const LauncherLanguage& language : LANGUAGES)
  {
    if (language.code != "system" && normalized.starts_with(std::string(language.code) + "-"))
      return std::string(language.code);
  }
  return "en";
}

std::vector<char> ReadBinaryFile(const std::string& path)
{
  File::IOFile file(path, "rb");
  const std::uint64_t size = file.GetSize();
  if (!file || size == 0 || size > std::numeric_limits<std::size_t>::max())
    return {};
  std::vector<char> data(static_cast<std::size_t>(size));
  if (!file.ReadBytes(data.data(), data.size()))
    return {};
  return data;
}
}  // namespace

std::size_t Localization::TransparentHash::operator()(std::string_view value) const noexcept
{
  return std::hash<std::string_view>{}(value);
}

std::span<const LauncherLanguage> Localization::GetLanguages()
{
  return LANGUAGES;
}

int Localization::FindLanguage(std::string_view code)
{
  const std::string normalized = NormalizeCode(code);
  for (std::size_t index = 0; index < LANGUAGES.size(); ++index)
  {
    if (normalized == NormalizeCode(LANGUAGES[index].code))
      return static_cast<int>(index);
  }
  return 0;
}

bool Localization::SetLanguage(std::string_view preference)
{
  int language_index = FindLanguage(preference);
  m_preference = std::string(LANGUAGES[language_index].code);
  m_resolved_code = m_preference == "system" ?
                        ResolveSupportedCode(GetSystemLanguageDefaults().locale) :
                        ResolveSupportedCode(m_preference);

  m_translations.clear();
  if (m_resolved_code != "en")
    LoadMoCatalog("romfs:/i18n/" + m_resolved_code + ".mo");
  AddLauncherTranslations();
  LoadJsonOverrides("romfs:/i18n/launcher/" + m_resolved_code + ".json");
  LoadJsonOverrides("sdmc:/switch/dolphin/i18n/" + m_resolved_code + ".json");
  return true;
}

std::string_view Localization::Translate(std::string_view source) const
{
  const auto found = m_translations.find(source);
  return found == m_translations.end() || found->second.empty() ? source : found->second;
}

std::string Localization::GetDisplayName() const
{
  const int resolved_index = FindLanguage(m_resolved_code);
  const std::string_view resolved_name = LANGUAGES[resolved_index].name;
  if (m_preference != "system")
    return std::string(resolved_name);
  return std::string(Translate("System")) + " (" + std::string(resolved_name) + ")";
}

LauncherFontFamily Localization::GetFontFamily() const
{
  if (m_resolved_code == "zh-CN")
    return LauncherFontFamily::SimplifiedChinese;
  if (m_resolved_code == "zh-TW")
    return LauncherFontFamily::TraditionalChinese;
  if (m_resolved_code == "ko")
    return LauncherFontFamily::Korean;
  return LauncherFontFamily::Standard;
}

void Localization::LoadMoCatalog(const std::string& path)
{
  const std::vector<char> data = ReadBinaryFile(path);
  constexpr std::uint32_t magic = 0x950412de;
  if (data.size() < 28)
    return;

  const auto read_u32 = [&](std::size_t offset, std::uint32_t* output) {
    if (!output || offset > data.size() - sizeof(std::uint32_t))
      return false;
    std::memcpy(output, data.data() + offset, sizeof(*output));
    return true;
  };
  std::uint32_t file_magic = 0;
  std::uint32_t count = 0;
  std::uint32_t original_table = 0;
  std::uint32_t translated_table = 0;
  if (!read_u32(0, &file_magic) || file_magic != magic || !read_u32(8, &count) ||
      !read_u32(12, &original_table) || !read_u32(16, &translated_table) || count > 100000 ||
      static_cast<std::uint64_t>(original_table) + static_cast<std::uint64_t>(count) * 8 >
          data.size() ||
      static_cast<std::uint64_t>(translated_table) + static_cast<std::uint64_t>(count) * 8 >
          data.size())
  {
    return;
  }

  for (std::uint32_t index = 0; index < count; ++index)
  {
    std::uint32_t original_length = 0;
    std::uint32_t original_offset = 0;
    std::uint32_t translated_length = 0;
    std::uint32_t translated_offset = 0;
    if (!read_u32(original_table + index * 8, &original_length) ||
        !read_u32(original_table + index * 8 + 4, &original_offset) ||
        !read_u32(translated_table + index * 8, &translated_length) ||
        !read_u32(translated_table + index * 8 + 4, &translated_offset) ||
        static_cast<std::uint64_t>(original_offset) + original_length > data.size() ||
        static_cast<std::uint64_t>(translated_offset) + translated_length > data.size() ||
        original_length == 0 || translated_length == 0)
    {
      continue;
    }

    std::string_view original(data.data() + original_offset, original_length);
    std::string_view translated(data.data() + translated_offset, translated_length);
    if (original.find('\4') != std::string_view::npos)
      continue;
    if (const std::size_t plural = original.find('\0'); plural != std::string_view::npos)
      original = original.substr(0, plural);
    if (const std::size_t plural = translated.find('\0'); plural != std::string_view::npos)
      translated = translated.substr(0, plural);
    if (!original.empty() && !translated.empty())
      m_translations.insert_or_assign(std::string(original), std::string(translated));
  }
}

void Localization::LoadJsonOverrides(const std::string& path)
{
  if (!File::Exists(path))
    return;
  picojson::value root;
  std::string error;
  if (!JsonFromFile(path, &root, &error) || !root.is<picojson::object>())
    return;
  for (const auto& [source, value] : root.get<picojson::object>())
  {
    if (value.is<std::string>() && !source.empty() && !value.get<std::string>().empty())
      m_translations.insert_or_assign(source, value.get<std::string>());
  }
}

void Localization::AddLauncherTranslations()
{
  int column = 0;
  if (m_resolved_code == "fr")
    column = 1;
  else if (m_resolved_code == "de")
    column = 2;
  else if (m_resolved_code == "es")
    column = 3;
  else if (m_resolved_code == "it")
    column = 4;
  else if (m_resolved_code == "pt")
    column = 5;
  if (column == 0)
    return;

  for (const LauncherTranslation& translation : LAUNCHER_TRANSLATIONS)
  {
    const std::array values = {translation.source, translation.fr, translation.de,
                               translation.es,     translation.it, translation.pt};
    m_translations.try_emplace(std::string(translation.source), std::string(values[column]));
  }
}
}  // namespace DolphinSwitch
