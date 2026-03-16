import { useState, useEffect } from 'preact/hooks'
import { api } from '../api'
import { t } from '../i18n'

const MODES = [
  { id: 0, key: 'modeOff' },
  { id: 1, key: 'modeTravel' },
  { id: 2, key: 'modeRandom' },
  { id: 3, key: 'modeSeq' },
]

const TRAVEL = [
  { id: 0, key: 'nearest' },
  { id: 1, key: 'axis' },
  { id: 2, key: 'random' },
]

export default function PlayTab({ orchState, tones, onRefresh, toast }) {
  useEffect(() => { onRefresh() }, [])

  function setMode(mode) {
    api('/api/orch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mode }),
    }).then(() => onRefresh())
  }

  function setTravel(order) {
    api('/api/orch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ travel_order: order }),
    }).then(() => onRefresh())
  }

  return (
    <div>
      <div className="bg-[#1a1a2e] border border-gray-700 rounded-lg p-3 mb-2.5">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-2">{t('mode')}</h3>
        <div className="grid grid-cols-4 gap-2">
          {MODES.map(m => (
            <button key={m.id} onClick={() => setMode(m.id)}
              className={`px-3 py-1.5 rounded-lg text-sm border ${orchState.mode === m.id
                ? 'border-cyan-400 text-cyan-400 bg-cyan-400/10'
                : 'border-gray-700 text-gray-200 bg-transparent'}`}>
              {t(m.key)}
            </button>
          ))}
        </div>
      </div>

      {orchState.mode === 1 && (
        <div className="bg-[#1a1a2e] border border-gray-700 rounded-lg p-3 mb-2.5">
          <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-2">{t('travel')}</h3>
          <div className="grid grid-cols-3 gap-2">
            {TRAVEL.map(tr => (
              <button key={tr.id} onClick={() => setTravel(tr.id)}
                className={`px-3 py-1.5 rounded-lg text-sm border ${orchState.travel_order === tr.id
                  ? 'border-cyan-400 text-cyan-400 bg-cyan-400/10'
                  : 'border-gray-700 text-gray-200 bg-transparent'}`}>
                {t(tr.key)}
              </button>
            ))}
          </div>
        </div>
      )}

      <div className="bg-[#1a1a2e] border border-gray-700 rounded-lg p-3">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-2">{t('tones')}</h3>
        <div className="grid grid-cols-3 gap-2">
          {tones.length === 0 && <span className="text-gray-500 text-sm col-span-3">{t('noTones')}</span>}
          {tones.map(tn => (
            <button key={tn.idx} onClick={() => toast(`${tn.name} ♫`)}
              className="px-3 py-1.5 rounded-lg text-sm border border-gray-700 text-gray-200 bg-transparent">
              {tn.name}
            </button>
          ))}
        </div>
      </div>
    </div>
  )
}
