import { signal, computed } from '@preact/signals'

const strings = {
  en: {
    tabMap: 'Map', tabPlay: 'Play', tabSeq: 'Seq', tabConfig: 'Config',
    battery: 'Battery', peers: 'peers', noNodes: 'No nodes yet',
    buildMap: 'Build Map', scanning: 'Scanning...', needNodes: 'Need 2+ nodes',
    mode: 'Mode', modeOff: 'Off', modeTravel: 'Travel', modeRandom: 'Random',
    modeSeq: 'Seq', modeSched: 'Sched', travel: 'Travel Order',
    nearest: 'Nearest', axis: 'Axis', random: 'Random', tones: 'Tones',
    start: 'Play', stop: 'Stop', addStep: 'Add Step', clear: 'Clear',
    save: 'Save', load: 'Load', node: 'Node', tone: 'Tone', delay: 'Delay ms',
    settings: 'Settings', wifi: 'WiFi', connected: 'Connected',
    notConnected: 'Not connected', storage: 'Storage', reboot: 'Reboot',
    confirmReboot: 'Reboot device?', version: 'Version', uptime: 'Uptime',
    sequence: 'Sequence', noTones: 'No tones',
  },
  fr: {
    tabMap: 'Carte', tabPlay: 'Jouer', tabSeq: 'Séq', tabConfig: 'Config',
    battery: 'Batterie', peers: 'pairs', noNodes: 'Aucun nœud',
    buildMap: 'Construire', scanning: 'Scan en cours…', needNodes: '2+ nœuds requis',
    mode: 'Mode', modeOff: 'Arrêt', modeTravel: 'Trajet', modeRandom: 'Aléatoire',
    modeSeq: 'Séq', modeSched: 'Planif', travel: 'Ordre trajet',
    nearest: 'Proche', axis: 'Axe', random: 'Aléatoire', tones: 'Sons',
    start: 'Jouer', stop: 'Arrêt', addStep: 'Ajouter', clear: 'Vider',
    save: 'Sauver', load: 'Charger', node: 'Nœud', tone: 'Son', delay: 'Délai ms',
    settings: 'Paramètres', wifi: 'WiFi', connected: 'Connecté',
    notConnected: 'Non connecté', storage: 'Stockage', reboot: 'Redémarrer',
    confirmReboot: 'Redémarrer l\'appareil ?', version: 'Version', uptime: 'Durée',
    sequence: 'Séquence', noTones: 'Aucun son',
  },
  es: {
    tabMap: 'Mapa', tabPlay: 'Tocar', tabSeq: 'Sec', tabConfig: 'Config',
    battery: 'Batería', peers: 'pares', noNodes: 'Sin nodos',
    buildMap: 'Construir mapa', scanning: 'Escaneando…', needNodes: '2+ nodos necesarios',
    mode: 'Modo', modeOff: 'Apagado', modeTravel: 'Viaje', modeRandom: 'Aleatorio',
    modeSeq: 'Sec', modeSched: 'Prog', travel: 'Orden viaje',
    nearest: 'Cercano', axis: 'Eje', random: 'Aleatorio', tones: 'Tonos',
    start: 'Tocar', stop: 'Parar', addStep: 'Agregar', clear: 'Limpiar',
    save: 'Guardar', load: 'Cargar', node: 'Nodo', tone: 'Tono', delay: 'Retardo ms',
    settings: 'Ajustes', wifi: 'WiFi', connected: 'Conectado',
    notConnected: 'Sin conexión', storage: 'Almacén', reboot: 'Reiniciar',
    confirmReboot: '¿Reiniciar dispositivo?', version: 'Versión', uptime: 'Tiempo',
    sequence: 'Secuencia', noTones: 'Sin tonos',
  },
  de: {
    tabMap: 'Karte', tabPlay: 'Spielen', tabSeq: 'Seq', tabConfig: 'Config',
    battery: 'Batterie', peers: 'Peers', noNodes: 'Keine Knoten',
    buildMap: 'Karte bauen', scanning: 'Scanne…', needNodes: '2+ Knoten nötig',
    mode: 'Modus', modeOff: 'Aus', modeTravel: 'Reise', modeRandom: 'Zufall',
    modeSeq: 'Seq', modeSched: 'Plan', travel: 'Reisefolge',
    nearest: 'Nächster', axis: 'Achse', random: 'Zufall', tones: 'Töne',
    start: 'Spielen', stop: 'Stopp', addStep: 'Hinzufügen', clear: 'Leeren',
    save: 'Speichern', load: 'Laden', node: 'Knoten', tone: 'Ton', delay: 'Verzög. ms',
    settings: 'Einstellungen', wifi: 'WLAN', connected: 'Verbunden',
    notConnected: 'Getrennt', storage: 'Speicher', reboot: 'Neustart',
    confirmReboot: 'Gerät neustarten?', version: 'Version', uptime: 'Laufzeit',
    sequence: 'Sequenz', noTones: 'Keine Töne',
  },
}

export const langs = ['en', 'fr', 'es', 'de']
export const flags = ['\uD83C\uDDEC\uD83C\uDDE7', '\uD83C\uDDEB\uD83C\uDDF7', '\uD83C\uDDEA\uD83C\uDDF8', '\uD83C\uDDE9\uD83C\uDDEA']

const stored = localStorage.getItem('sqlang') || navigator.language.slice(0, 2)
export const lang = signal(langs.includes(stored) ? stored : 'en')

export function t(k) {
  const l = lang.value
  return strings[l]?.[k] || strings.en[k] || k
}

export function cycleLang() {
  const idx = (langs.indexOf(lang.value) + 1) % langs.length
  lang.value = langs[idx]
  localStorage.setItem('sqlang', langs[idx])
}

export const langFlag = computed(() => {
  const idx = langs.indexOf(lang.value)
  return flags[idx >= 0 ? idx : 0]
})
