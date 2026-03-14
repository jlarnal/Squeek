#include "i18n_strings.h"

// This file MUST be saved as UTF-8. The static_assert below will fire if
// the compiler's input charset mangles the French 'é' (U+00E9) into
// anything other than the two-byte UTF-8 sequence 0xC3 0xA9.
static constexpr bool isUtf8() {
    const char* s = "é";
    return static_cast<unsigned char>(s[0]) == 0xC3
        && static_cast<unsigned char>(s[1]) == 0xA9
        && s[2] == '\0';
}
static_assert(isUtf8(), "i18n_strings.cpp is not UTF-8 encoded — re-save as UTF-8");

// ---- Settings Hash (private, not user-visible) ----

static const char* const NAME_SETTINGS_HASH[] = {
    "Settings Hash",
    "Empreinte des réglages",
    "Hash de configuración",
    "Einstellungs-Hash",
    nullptr
};
static const char* const DESC_SETTINGS_HASH[] = {
    "Compile-time hash of default settings",
    "Empreinte des réglages par défaut à la compilation",
    "Hash de la configuración predeterminada en compilación",
    "Kompilierzeit-Hash der Standardeinstellungen",
    nullptr
};
const PropStrings I18N_SETTINGS_HASH = { NAME_SETTINGS_HASH, DESC_SETTINGS_HASH };

// ---- LEDs Enabled ----

static const char* const NAME_LEDS_ENABLED[] = {
    "LEDs Enabled",
    "LEDs activées",
    "LEDs habilitados",
    "LEDs aktiviert",
    nullptr
};
static const char* const DESC_LEDS_ENABLED[] = {
    "Use status and RGB LEDs",
    "Utiliser les LEDs RGB et de statut",
    "Usar las LEDs de estado y RGB",
    "Status- und RGB-LEDs verwenden",
    nullptr
};
const PropStrings I18N_LEDS_ENABLED = { NAME_LEDS_ENABLED, DESC_LEDS_ENABLED };

// ---- Color: Init ----

static const char* const NAME_COLOR_INIT[] = {
    "Init Color",
    "Couleur initiale",
    "Color inicial",
    "Startfarbe",
    nullptr
};
static const char* const DESC_COLOR_INIT[] = {
    "LED color during initialization (0xRRGGBB)",
    "Couleur des LEDs au démarrage (0xRRGGBB)",
    "Color de LEDs durante la inicialización (0xRRGGBB)",
    "LED-Farbe bei der Initialisierung (0xRRGGBB)",
    nullptr
};
const PropStrings I18N_COLOR_INIT = { NAME_COLOR_INIT, DESC_COLOR_INIT };

// ---- Color: Ready ----

static const char* const NAME_COLOR_READY[] = {
    "Ready Color",
    "Couleur prêt",
    "Color listo",
    "Bereitfarbe",
    nullptr
};
static const char* const DESC_COLOR_READY[] = {
    "LED color when mesh is ready",
    "Couleur des LEDs quand le réseau maillé est prêt",
    "Color de LEDs cuando la red está lista",
    "LED-Farbe bei bereitem Mesh-Netzwerk",
    nullptr
};
const PropStrings I18N_COLOR_READY = { NAME_COLOR_READY, DESC_COLOR_READY };

// ---- Color: Gateway ----

static const char* const NAME_COLOR_GATEWAY[] = {
    "Gateway Color",
    "Couleur passerelle",
    "Color pasarela",
    "Gateway-Farbe",
    nullptr
};
static const char* const DESC_COLOR_GATEWAY[] = {
    "LED color for gateway role",
    "Couleur des LEDs en rôle passerelle",
    "Color de LEDs en rol de pasarela",
    "LED-Farbe als Gateway",
    nullptr
};
const PropStrings I18N_COLOR_GATEWAY = { NAME_COLOR_GATEWAY, DESC_COLOR_GATEWAY };

// ---- Color: Peer ----

static const char* const NAME_COLOR_PEER[] = {
    "Peer Color",
    "Couleur pair",
    "Color par",
    "Peer-Farbe",
    nullptr
};
static const char* const DESC_COLOR_PEER[] = {
    "LED color for peer role",
    "Couleur des LEDs en rôle pair",
    "Color de LEDs en rol de par",
    "LED-Farbe als Peer",
    nullptr
};
const PropStrings I18N_COLOR_PEER = { NAME_COLOR_PEER, DESC_COLOR_PEER };

// ---- Color: Disconnected ----

static const char* const NAME_COLOR_DISCONNECTED[] = {
    "Disconnected Color",
    "Couleur déconnecté",
    "Color desconectado",
    "Farbe getrennt",
    nullptr
};
static const char* const DESC_COLOR_DISCONNECTED[] = {
    "LED color when disconnected from mesh",
    "Couleur des LEDs en cas de déconnexion du réseau",
    "Color de LEDs al perder conexión con la red",
    "LED-Farbe bei Verbindungsverlust",
    nullptr
};
const PropStrings I18N_COLOR_DISCONNECTED = { NAME_COLOR_DISCONNECTED, DESC_COLOR_DISCONNECTED };

// ---- Heartbeat Interval ----

static const char* const NAME_HEARTBEAT_INTERVAL[] = {
    "Heartbeat Interval",
    "Intervalle de battement",
    "Intervalo de latido",
    "Heartbeat-Intervall",
    nullptr
};
static const char* const DESC_HEARTBEAT_INTERVAL[] = {
    "Seconds between heartbeat messages",
    "Secondes entre les messages de battement",
    "Segundos entre mensajes de latido",
    "Sekunden zwischen Heartbeat-Nachrichten",
    nullptr
};
const PropStrings I18N_HEARTBEAT_INTERVAL = { NAME_HEARTBEAT_INTERVAL, DESC_HEARTBEAT_INTERVAL };

// ---- Heartbeat Stale Multiplier ----

static const char* const NAME_HEARTBEAT_STALE[] = {
    "Stale Multiplier",
    "Multiplicateur d'expiration",
    "Multiplicador de expiración",
    "Veraltungs-Multiplikator",
    nullptr
};
static const char* const DESC_HEARTBEAT_STALE[] = {
    "Missed heartbeats before marking a peer as stale",
    "Battements manqués avant de considérer un pair comme inactif",
    "Latidos perdidos antes de marcar un par como inactivo",
    "Verpasste Heartbeats bis ein Peer als veraltet gilt",
    nullptr
};
const PropStrings I18N_HEARTBEAT_STALE = { NAME_HEARTBEAT_STALE, DESC_HEARTBEAT_STALE };

// ---- Re-election Cooldown ----

static const char* const NAME_REELECTION_COOLDOWN[] = {
    "Re-election Cooldown",
    "Délai de réélection",
    "Espera de reelección",
    "Neuwahlpause",
    nullptr
};
static const char* const DESC_REELECTION_COOLDOWN[] = {
    "Seconds before allowing a new gateway election",
    "Secondes avant d'autoriser une nouvelle élection de passerelle",
    "Segundos antes de permitir una nueva elección de pasarela",
    "Sekunden vor einer erneuten Gateway-Wahl",
    nullptr
};
const PropStrings I18N_REELECTION_COOLDOWN = { NAME_REELECTION_COOLDOWN, DESC_REELECTION_COOLDOWN };

// ---- Battery Hysteresis ----

static const char* const NAME_BATTERY_HYSTERESIS[] = {
    "Battery Hysteresis",
    "Hystérésis batterie",
    "Histéresis de batería",
    "Batterie-Hysterese",
    nullptr
};
static const char* const DESC_BATTERY_HYSTERESIS[] = {
    "Millivolt threshold for battery-based role rotation",
    "Seuil en millivolts pour la rotation de rôle sur batterie",
    "Umbral en milivoltios para rotación de rol por batería",
    "Millivolt-Schwelle für batteriebasierten Rollenwechsel",
    nullptr
};
const PropStrings I18N_BATTERY_HYSTERESIS = { NAME_BATTERY_HYSTERESIS, DESC_BATTERY_HYSTERESIS };

// ---- FTM Anchors ----

static const char* const NAME_FTM_ANCHORS[] = {
    "FTM Anchors",
    "Ancres FTM",
    "Anclas FTM",
    "FTM-Anker",
    nullptr
};
static const char* const DESC_FTM_ANCHORS[] = {
    "Anchor nodes used for new-node FTM ranging",
    "Nœuds d'ancrage utilisés pour la télémétrie FTM des nouveaux nœuds",
    "Nodos ancla usados para telemetría FTM de nodos nuevos",
    "Ankerknoten für die FTM-Entfernungsmessung neuer Knoten",
    nullptr
};
const PropStrings I18N_FTM_ANCHORS = { NAME_FTM_ANCHORS, DESC_FTM_ANCHORS };

// ---- FTM Samples ----

static const char* const NAME_FTM_SAMPLES[] = {
    "FTM Samples",
    "Échantillons FTM",
    "Muestras FTM",
    "FTM-Messungen",
    nullptr
};
static const char* const DESC_FTM_SAMPLES[] = {
    "Measurement samples per node pair",
    "Mesures par paire de nœuds",
    "Mediciones por par de nodos",
    "Messproben pro Knotenpaar",
    nullptr
};
const PropStrings I18N_FTM_SAMPLES = { NAME_FTM_SAMPLES, DESC_FTM_SAMPLES };

// ---- FTM Timeout ----

static const char* const NAME_FTM_TIMEOUT[] = {
    "FTM Timeout",
    "Délai FTM",
    "Tiempo límite FTM",
    "FTM-Zeitlimit",
    nullptr
};
static const char* const DESC_FTM_TIMEOUT[] = {
    "Timeout per FTM pair in milliseconds",
    "Délai d'attente par paire FTM en millisecondes",
    "Tiempo de espera por par FTM en milisegundos",
    "Zeitlimit pro FTM-Paar in Millisekunden",
    nullptr
};
const PropStrings I18N_FTM_TIMEOUT = { NAME_FTM_TIMEOUT, DESC_FTM_TIMEOUT };

// ---- FTM Kalman Noise ----

static const char* const NAME_FTM_KALMAN_NOISE[] = {
    "FTM Kalman Noise",
    "Bruit Kalman FTM",
    "Ruido Kalman FTM",
    "FTM-Kalman-Rauschen",
    nullptr
};
static const char* const DESC_FTM_KALMAN_NOISE[] = {
    "Process noise for FTM Kalman filter",
    "Bruit de processus du filtre de Kalman FTM",
    "Ruido de proceso del filtro Kalman FTM",
    "Prozessrauschen des FTM-Kalman-Filters",
    nullptr
};
const PropStrings I18N_FTM_KALMAN_NOISE = { NAME_FTM_KALMAN_NOISE, DESC_FTM_KALMAN_NOISE };

// ---- FTM Offset ----

static const char* const NAME_FTM_OFFSET[] = {
    "FTM Offset",
    "Décalage FTM",
    "Compensación FTM",
    "FTM-Versatz",
    nullptr
};
static const char* const DESC_FTM_OFFSET[] = {
    "FTM responder distance offset in centimeters",
    "Décalage de distance du répondeur FTM en centimètres",
    "Compensación de distancia del respondedor FTM en centímetros",
    "FTM-Responder-Entfernungsversatz in Zentimetern",
    nullptr
};
const PropStrings I18N_FTM_OFFSET = { NAME_FTM_OFFSET, DESC_FTM_OFFSET };

// ---- Orchestrator Mode ----

static const char* const NAME_ORCH_MODE[] = {
    "Orchestrator Mode",
    "Mode d'orchestration",
    "Modo de orquestación",
    "Orchestrierungsmodus",
    nullptr
};
static const char* const DESC_ORCH_MODE[] = {
    "Active play mode (0=travel, 1=random, 2=sequence, 3=scheduled)",
    "Mode de jeu actif (0=parcours, 1=aléatoire, 2=séquence, 3=programmé)",
    "Modo de juego activo (0=recorrido, 1=aleatorio, 2=secuencia, 3=programado)",
    "Aktiver Spielmodus (0=Wandern, 1=Zufall, 2=Sequenz, 3=Geplant)",
    nullptr
};
const PropStrings I18N_ORCH_MODE = { NAME_ORCH_MODE, DESC_ORCH_MODE };

// ---- Orchestrator Travel Delay ----

static const char* const NAME_ORCH_TRAVEL_DELAY[] = {
    "Travel Delay",
    "Délai de parcours",
    "Retardo de recorrido",
    "Wanderverzögerung",
    nullptr
};
static const char* const DESC_ORCH_TRAVEL_DELAY[] = {
    "Delay between travel mode steps in milliseconds",
    "Délai entre les étapes du mode parcours en millisecondes",
    "Retardo entre pasos del modo recorrido en milisegundos",
    "Verzögerung zwischen Wanderschritten in Millisekunden",
    nullptr
};
const PropStrings I18N_ORCH_TRAVEL_DELAY = { NAME_ORCH_TRAVEL_DELAY, DESC_ORCH_TRAVEL_DELAY };

// ---- Orchestrator Random Min ----

static const char* const NAME_ORCH_RANDOM_MIN[] = {
    "Random Min Delay",
    "Délai min. aléatoire",
    "Retardo mín. aleatorio",
    "Min. Zufallspause",
    nullptr
};
static const char* const DESC_ORCH_RANDOM_MIN[] = {
    "Minimum delay for random popup mode in milliseconds",
    "Délai minimum du mode apparition aléatoire en millisecondes",
    "Retardo mínimo del modo aparición aleatoria en milisegundos",
    "Minimale Pause im Zufallsmodus in Millisekunden",
    nullptr
};
const PropStrings I18N_ORCH_RANDOM_MIN = { NAME_ORCH_RANDOM_MIN, DESC_ORCH_RANDOM_MIN };

// ---- Orchestrator Random Max ----

static const char* const NAME_ORCH_RANDOM_MAX[] = {
    "Random Max Delay",
    "Délai max. aléatoire",
    "Retardo máx. aleatorio",
    "Max. Zufallspause",
    nullptr
};
static const char* const DESC_ORCH_RANDOM_MAX[] = {
    "Maximum delay for random popup mode in milliseconds",
    "Délai maximum du mode apparition aléatoire en millisecondes",
    "Retardo máximo del modo aparición aleatoria en milisegundos",
    "Maximale Pause im Zufallsmodus in Millisekunden",
    nullptr
};
const PropStrings I18N_ORCH_RANDOM_MAX = { NAME_ORCH_RANDOM_MAX, DESC_ORCH_RANDOM_MAX };

// ---- Orchestrator Tone Index ----

static const char* const NAME_ORCH_TONE_INDEX[] = {
    "Tone Index",
    "Index de tonalité",
    "Índice de tono",
    "Ton-Index",
    nullptr
};
static const char* const DESC_ORCH_TONE_INDEX[] = {
    "Default tone to play (index into tone library)",
    "Tonalité par défaut (index dans la bibliothèque de sons)",
    "Tono predeterminado (índice en la biblioteca de sonidos)",
    "Standardton (Index in der Tonbibliothek)",
    nullptr
};
const PropStrings I18N_ORCH_TONE_INDEX = { NAME_ORCH_TONE_INDEX, DESC_ORCH_TONE_INDEX };

// ---- Clock Sync Interval ----

static const char* const NAME_CLOCK_SYNC_INTERVAL[] = {
    "Clock Sync Interval",
    "Intervalle de synchronisation",
    "Intervalo de sincronización",
    "Taktsynchronisierung",
    nullptr
};
static const char* const DESC_CLOCK_SYNC_INTERVAL[] = {
    "Seconds between clock synchronization broadcasts",
    "Secondes entre les diffusions de synchronisation d'horloge",
    "Segundos entre difusiones de sincronización de reloj",
    "Sekunden zwischen Uhrzeitsynchronisations-Broadcasts",
    nullptr
};
const PropStrings I18N_CLOCK_SYNC_INTERVAL = { NAME_CLOCK_SYNC_INTERVAL, DESC_CLOCK_SYNC_INTERVAL };

// ---- Web UI Enabled ----

static const char* const NAME_WEB_ENABLED[] = {
    "Web UI Enabled",
    "Interface Web activée",
    "Interfaz Web habilitada",
    "Web-UI aktiviert",
    nullptr
};
static const char* const DESC_WEB_ENABLED[] = {
    "Enable the built-in web dashboard",
    "Activer le tableau de bord Web intégré",
    "Habilitar el panel Web integrado",
    "Integriertes Web-Dashboard aktivieren",
    nullptr
};
const PropStrings I18N_WEB_ENABLED = { NAME_WEB_ENABLED, DESC_WEB_ENABLED };

// ---- Fast-Scan Delay ----

static const char* const NAME_FAST_SCAN_DELAY[] = {
    "Fast-Scan Delay",
    "Durée du balayage rapide",
    "Duración de escaneo rápido",
    "Schnellscan-Dauer",
    nullptr
};
static const char* const DESC_FAST_SCAN_DELAY[] = {
    "Seconds of fast scanning on boot before slowing down",
    "Secondes de balayage rapide au démarrage avant de ralentir",
    "Segundos de escaneo rápido al arrancar antes de reducir velocidad",
    "Sekunden Schnellscan beim Start bevor verlangsamt wird",
    nullptr
};
const PropStrings I18N_FAST_SCAN_DELAY = { NAME_FAST_SCAN_DELAY, DESC_FAST_SCAN_DELAY };

// ---- Delegate Timeout ----

static const char* const NAME_DELEGATE_TIMEOUT[] = {
    "Delegate Timeout",
    "Délai du délégué",
    "Tiempo del delegado",
    "Delegat-Zeitlimit",
    nullptr
};
static const char* const DESC_DELEGATE_TIMEOUT[] = {
    "Seconds before the setup delegate gives up",
    "Secondes avant que le délégué de configuration abandonne",
    "Segundos antes de que el delegado de configuración desista",
    "Sekunden bevor der Einrichtungsdelegat aufgibt",
    nullptr
};
const PropStrings I18N_DELEGATE_TIMEOUT = { NAME_DELEGATE_TIMEOUT, DESC_DELEGATE_TIMEOUT };

// ---- RSSI Decay K ----

static const char* const NAME_RSSI_DECAY_K[] = {
    "RSSI Decay K",
    "Lissage RSSI",
    "Suavizado RSSI",
    "RSSI-Glättung",
    nullptr
};
static const char* const DESC_RSSI_DECAY_K[] = {
    "RSSI smoothing factor (Q4.4 fixed-point)",
    "Facteur de lissage du RSSI (virgule fixe Q4.4)",
    "Factor de suavizado del RSSI (punto fijo Q4.4)",
    "RSSI-Glättungsfaktor (Q4.4-Festkomma)",
    nullptr
};
const PropStrings I18N_RSSI_DECAY_K = { NAME_RSSI_DECAY_K, DESC_RSSI_DECAY_K };

// ---- Battery in Tenure ----

static const char* const NAME_BATTERY_IN_TENURE[] = {
    "Battery in Tenure",
    "Batterie dans le score",
    "Batería en puntuación",
    "Batterie in Bewertung",
    nullptr
};
static const char* const DESC_BATTERY_IN_TENURE[] = {
    "Include battery level in tenure score calculation",
    "Inclure le niveau de batterie dans le calcul du score de tenure",
    "Incluir nivel de batería en el cálculo de puntuación de tenencia",
    "Batteriestand in die Tenure-Bewertung einbeziehen",
    nullptr
};
const PropStrings I18N_BATTERY_IN_TENURE = { NAME_BATTERY_IN_TENURE, DESC_BATTERY_IN_TENURE };

// ---- Election Slot ----

static const char* const NAME_ELECTION_SLOT[] = {
    "Election Slot",
    "Créneau d'élection",
    "Ranura de elección",
    "Wahlslot",
    nullptr
};
static const char* const DESC_ELECTION_SLOT[] = {
    "Duration of each election slot in milliseconds",
    "Durée de chaque créneau d'élection en millisecondes",
    "Duración de cada ranura de elección en milisegundos",
    "Dauer jedes Wahlslots in Millisekunden",
    nullptr
};
const PropStrings I18N_ELECTION_SLOT = { NAME_ELECTION_SLOT, DESC_ELECTION_SLOT };

// ---- Election Announce ----

static const char* const NAME_ELECTION_ANNOUNCE[] = {
    "Election Announce",
    "Annonce d'élection",
    "Anuncio de elección",
    "Wahlankündigung",
    nullptr
};
static const char* const DESC_ELECTION_ANNOUNCE[] = {
    "Duration of the election announcement phase in milliseconds",
    "Durée de la phase d'annonce d'élection en millisecondes",
    "Duración de la fase de anuncio de elección en milisegundos",
    "Dauer der Wahlankündigungsphase in Millisekunden",
    nullptr
};
const PropStrings I18N_ELECTION_ANNOUNCE = { NAME_ELECTION_ANNOUNCE, DESC_ELECTION_ANNOUNCE };
