import { useState, useCallback } from 'preact/hooks'
import { api } from './api'
import { t, cycleLang, langFlag, lang } from './i18n'
import { useWebSocket, wsConnected } from './hooks/useWebSocket'
import { useToast } from './hooks/useToast'
import MapTab from './tabs/MapTab'
import PlayTab from './tabs/PlayTab'
import SeqTab from './tabs/SeqTab'
import ConfigTab from './tabs/ConfigTab'

const UI_VERSION = '6.0'

const TAB_ICONS = {
  map: 'M12 2C8.13 2 5 5.13 5 9c0 5.25 7 13 7 13s7-7.75 7-13c0-3.87-3.13-7-7-7zm0 9.5a2.5 2.5 0 010-5 2.5 2.5 0 010 5z',
  play: 'M8 5v14l11-7z',
  seq: 'M3 13h2v-2H3v2zm0 4h2v-2H3v2zm0-8h2V7H3v2zm4 4h14v-2H7v2zm0 4h14v-2H7v2zM7 7v2h14V7H7z',
  config: 'M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58a.49.49 0 00.12-.61l-1.92-3.32a.49.49 0 00-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54a.484.484 0 00-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.07.62-.07.94s.02.64.07.94l-2.03 1.58a.49.49 0 00-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6A3.6 3.6 0 1115.6 12 3.6 3.6 0 0112 15.6z',
}

const TABS = [
  { id: 'map', key: 'tabMap', icon: TAB_ICONS.map },
  { id: 'play', key: 'tabPlay', icon: TAB_ICONS.play },
  { id: 'seq', key: 'tabSeq', icon: TAB_ICONS.seq },
  { id: 'config', key: 'tabConfig', icon: TAB_ICONS.config },
]

export default function App() {
  const [tab, setTab] = useState('map')
  const [peers, setPeers] = useState([])
  const [distances, setDistances] = useState([])
  const [status, setStatus] = useState({})
  const [orchState, setOrchState] = useState({ mode: 0, travel_order: 0, sequence: [] })
  const [tones, setTones] = useState([])
  const { msg: toastMsg, toast } = useToast()

  // Force re-read of lang signal for reactivity
  const _lang = lang.value

  const refreshMap = useCallback(() => {
    console.log('[app] refreshMap')
    Promise.all([api('/api/peers'), api('/api/distances'), api('/api/status')]).then(([p, d, s]) => {
      console.log('[app] refreshMap OK, peers=', p.length, 'distances=', d.length)
      setPeers(p); setDistances(d); setStatus(s)
    }).catch(err => { console.error('[app] refreshMap failed', err) })
  }, [])

  const refreshPlay = useCallback(() => {
    Promise.all([api('/api/orch'), api('/api/tones')]).then(([o, tn]) => {
      setOrchState(o); setTones(tn)
    }).catch(() => {})
  }, [])

  const refreshSeq = useCallback(() => {
    Promise.all([api('/api/orch'), api('/api/peers'), api('/api/tones')]).then(([o, p, tn]) => {
      setOrchState(o); setPeers(p); setTones(tn)
    }).catch(() => {})
  }, [])

  // WebSocket
  useWebSocket(useCallback((msg) => {
    if (msg.type === 'peer_join' || msg.type === 'peer_leave' || msg.type === 'peer_update') {
      refreshMap()
    }
    if (msg.type === 'orch_update') {
      setOrchState(prev => ({
        ...prev,
        mode: msg.mode,
        ...(msg.travel_order !== undefined ? { travel_order: msg.travel_order } : {}),
      }))
    }
  }, [refreshMap]))

  return (
    <div className="h-screen flex flex-col bg-[#0d0d1a] text-gray-200 select-none">
      {/* Header */}
      <header className="flex items-center justify-between px-4 py-2.5 bg-[#1a1a2e] border-b border-gray-700 shrink-0 z-10">
        <div className="flex items-center gap-2">
          <span className="text-xl">🐾</span>
          <h1 className="text-base font-semibold text-cyan-400 tracking-wide">Squeek</h1>
          <span className="text-[0.6em] text-gray-500 self-end mb-0.5">v{UI_VERSION}</span>
        </div>
        <div className="flex items-center gap-2">
          <div className={`w-2 h-2 rounded-full ${wsConnected.value ? 'bg-green-700' : 'bg-red-900'}`} title="WebSocket" />
          <button onClick={cycleLang}
            className="w-8 h-8 rounded-full border border-gray-700 bg-[#0d0d1a] text-base cursor-pointer flex items-center justify-center">
            {langFlag.value}
          </button>
        </div>
      </header>

      {/* Tab content */}
      <main className="flex-1 overflow-y-auto overflow-x-hidden p-3 pb-[72px]" style={{ WebkitOverflowScrolling: 'touch' }}>
        {tab === 'map' && <MapTab peers={peers} distances={distances} status={status} onRefresh={refreshMap} toast={toast} />}
        {tab === 'play' && <PlayTab orchState={orchState} tones={tones} onRefresh={refreshPlay} toast={toast} />}
        {tab === 'seq' && <SeqTab peers={peers} tones={tones} orchState={orchState} onRefresh={refreshSeq} toast={toast} />}
        {tab === 'config' && <ConfigTab toast={toast} />}
      </main>

      {/* Bottom nav */}
      <nav className="fixed bottom-0 left-0 right-0 h-14 bg-[#1a1a2e] border-t border-gray-700 grid grid-cols-4 z-10">
        {TABS.map(tb => (
          <button key={tb.id} onClick={() => setTab(tb.id)}
            className={`flex flex-col items-center justify-center gap-0.5 text-[0.65em] border-none bg-transparent cursor-pointer transition-colors ${
              tab === tb.id ? 'text-cyan-400' : 'text-gray-500'}`}>
            <svg viewBox="0 0 24 24" className="w-[22px] h-[22px] fill-current"><path d={tb.icon} /></svg>
            <span>{t(tb.key)}</span>
          </button>
        ))}
      </nav>

      {/* Toast */}
      {toastMsg && (
        <div className="fixed top-[60px] left-1/2 -translate-x-1/2 bg-[#252545] border border-gray-700 rounded-lg px-4 py-2 text-sm z-20">
          {toastMsg}
        </div>
      )}
    </div>
  )
}
