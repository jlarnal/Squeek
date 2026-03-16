import { useState, useEffect } from 'preact/hooks'
import { api } from '../api'
import { t } from '../i18n'

export default function SeqTab({ peers, tones, orchState, onRefresh, toast }) {
  const [steps, setSteps] = useState([])
  const [nodeVal, setNodeVal] = useState(0)
  const [toneVal, setToneVal] = useState(0)
  const [delayVal, setDelayVal] = useState(500)

  useEffect(() => {
    onRefresh()
  }, [])

  useEffect(() => {
    if (orchState.sequence) setSteps(orchState.sequence.slice())
  }, [orchState.sequence])

  function addStep() {
    if (steps.length >= 32) { toast('Max 32 steps'); return }
    setSteps([...steps, { node: nodeVal, tone: toneVal, delay: delayVal }])
  }

  function removeStep(i) {
    setSteps(steps.filter((_, idx) => idx !== i))
  }

  function save() {
    api('/api/sequence', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ steps, save: true }),
    }).then(r => toast(`${t('save')} (${r.steps})`))
  }

  function play() {
    api('/api/sequence', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ steps, save: false }),
    }).then(() =>
      api('/api/orch', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ mode: 3 }),
      })
    ).then(() => toast(t('start')))
  }

  return (
    <div>
      <div className="bg-[#1a1a2e] border border-gray-700 rounded-lg p-3 mb-2.5">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-2">{t('sequence')}</h3>
        <div>
          {steps.map((s, i) => {
            const peer = peers.find(p => p.idx === s.node)
            const tn = tones.find(tt => tt.idx === s.tone)
            return (
              <div key={i} className="grid gap-1.5 items-center py-1.5 border-b border-gray-700/50 text-sm"
                   style={{ gridTemplateColumns: '28px 1fr 1fr 70px 32px' }}>
                <span className="text-gray-500 text-center text-xs">{i + 1}</span>
                <span>{peer ? peer.mac.slice(-5) : `#${s.node}`}</span>
                <span>{tn ? tn.name : `T${s.tone}`}</span>
                <span>{s.delay} ms</span>
                <button onClick={() => removeStep(i)}
                  className="w-7 h-7 border-none bg-transparent text-red-400 cursor-pointer text-base rounded">
                  &times;
                </button>
              </div>
            )
          })}
        </div>
        <div className="text-center text-gray-500 text-xs mt-1.5">{steps.length}/32</div>
      </div>

      <div className="bg-[#1a1a2e] border border-gray-700 rounded-lg p-3 mb-2.5">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-2">{t('addStep')}</h3>
        <div className="flex flex-wrap gap-1.5">
          <div className="flex-1 min-w-[80px]">
            <label className="block text-xs text-gray-500 mb-1">{t('node')}</label>
            <select value={nodeVal} onChange={e => setNodeVal(parseInt(e.target.value))}
              className="w-full p-2 rounded-lg border border-gray-700 bg-[#0d0d1a] text-gray-200 outline-none text-sm">
              {peers.map(p => <option key={p.idx} value={p.idx}>#{p.idx} {p.mac.slice(-5)}</option>)}
            </select>
          </div>
          <div className="flex-1 min-w-[80px]">
            <label className="block text-xs text-gray-500 mb-1">{t('tone')}</label>
            <select value={toneVal} onChange={e => setToneVal(parseInt(e.target.value))}
              className="w-full p-2 rounded-lg border border-gray-700 bg-[#0d0d1a] text-gray-200 outline-none text-sm">
              {tones.map(tn => <option key={tn.idx} value={tn.idx}>{tn.name}</option>)}
            </select>
          </div>
          <div className="w-[70px]">
            <label className="block text-xs text-gray-500 mb-1">{t('delay')}</label>
            <input type="number" value={delayVal} onInput={e => setDelayVal(parseInt(e.target.value) || 0)}
              min="0" max="10000" step="50"
              className="w-full p-2 rounded-lg border border-gray-700 bg-[#0d0d1a] text-gray-200 outline-none text-sm" />
          </div>
          <button onClick={addStep}
            className="self-end px-3 py-2 rounded-lg bg-cyan-400 text-gray-950 text-sm font-semibold">
            +
          </button>
        </div>
      </div>

      <div className="grid grid-cols-4 gap-2">
        <button onClick={play} className="px-3 py-1.5 rounded-lg bg-cyan-400 text-gray-950 text-sm font-semibold">{t('start')}</button>
        <button onClick={save} className="px-3 py-1.5 rounded-lg border border-gray-700 text-gray-200 text-sm">{t('save')}</button>
        <button onClick={onRefresh} className="px-3 py-1.5 rounded-lg border border-gray-700 text-gray-200 text-sm">{t('load')}</button>
        <button onClick={() => setSteps([])} className="px-3 py-1.5 rounded-lg border border-gray-700 text-gray-200 text-sm">{t('clear')}</button>
      </div>
    </div>
  )
}
