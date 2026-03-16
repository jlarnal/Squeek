import { useState, useEffect } from 'preact/hooks'
import { api } from '../api'
import { t } from '../i18n'

function formatUptime(s) {
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60
  return (h ? `${h}h ` : '') + (m ? `${m}m ` : '') + `${sec}s`
}

export default function ConfigTab({ toast }) {
  const [meta, setMeta] = useState([])
  const [values, setValues] = useState({})
  const [sys, setSys] = useState({})
  const [storage, setStorage] = useState({})

  useEffect(() => { refresh() }, [])

  function refresh() {
    Promise.all([api('/api/config'), api('/api/status'), api('/api/storage')]).then(([cfg, st, stor]) => {
      const m = cfg._meta || []
      setMeta(m)
      const v = {}
      m.forEach(f => { v[f.key] = cfg[f.key] })
      setValues(v)
      setSys(st)
      setStorage(stor)
    })
  }

  function save() {
    const payload = {}
    meta.forEach(m => { payload[m.key] = values[m.key] })
    api('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }).then(r => toast(`${t('save')} (${r.applied})`))
  }

  function reboot() {
    if (!confirm(t('confirmReboot'))) return
    api('/api/reboot', { method: 'POST' }).then(() => toast(`${t('reboot')}...`))
  }

  function setVal(key, val) {
    setValues(prev => ({ ...prev, [key]: val }))
  }

  const pct = storage.total ? Math.round((storage.used / storage.total) * 100) : 0

  return (
    <div>
      <div className="bg-[#1a1a2e] border border-gray-700 rounded-lg p-3 mb-2.5">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-2">{t('settings')}</h3>
        <div>
          {meta.map(m => (
            <div key={m.key} className="flex items-center justify-between py-2 border-b border-gray-700/30 gap-2">
              <div className="flex-1 min-w-0">
                <div className="text-sm text-gray-200">{m.key}</div>
                <div className="text-xs text-gray-500 truncate">{m.desc}</div>
              </div>
              {m.type === 'bool' ? (
                <label className="relative w-11 h-6 shrink-0">
                  <input type="checkbox" checked={values[m.key] || false}
                    onChange={e => setVal(m.key, e.target.checked)}
                    className="sr-only peer" />
                  <span className="absolute inset-0 bg-gray-700 rounded-xl cursor-pointer transition-colors peer-checked:bg-cyan-400" />
                  <span className="absolute h-[18px] w-[18px] left-[3px] bottom-[3px] bg-gray-200 rounded-full transition-transform peer-checked:translate-x-5" />
                </label>
              ) : (
                <div className="w-[90px] shrink-0">
                  <input type="number" step={m.type === 'float' ? '0.01' : '1'}
                    value={values[m.key] ?? ''} onInput={e => {
                      const v = m.type === 'float' ? parseFloat(e.target.value) : parseInt(e.target.value)
                      setVal(m.key, v)
                    }}
                    className="w-full p-1.5 rounded-lg border border-gray-700 bg-[#0d0d1a] text-gray-200 text-right text-sm outline-none focus:border-cyan-400" />
                </div>
              )}
            </div>
          ))}
        </div>
        <button onClick={save}
          className="w-full mt-2.5 px-4 py-2.5 rounded-lg bg-cyan-400 text-gray-950 font-semibold text-sm">
          {t('save')}
        </button>
      </div>

      <div className="bg-[#1a1a2e] border border-gray-700 rounded-lg p-3 mb-2.5">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-2">System</h3>
        <div className="text-sm">
          {[
            [t('wifi'), sys.mac],
            [t('storage'), `${Math.round((storage.used || 0) / 1024)}/${Math.round((storage.total || 0) / 1024)} KB`],
            [t('uptime'), sys.uptime_s != null ? formatUptime(sys.uptime_s) : '--'],
            [t('version'), sys.build || '--'],
            ['Heap', sys.free_heap ? `${Math.round(sys.free_heap / 1024)} KB` : '--'],
          ].map(([lbl, val]) => (
            <div key={lbl} className="flex justify-between py-1">
              <span className="text-gray-500">{lbl}</span>
              <span>{val || '--'}</span>
            </div>
          ))}
        </div>
        <div className="h-1.5 bg-gray-700 rounded-full overflow-hidden mt-2">
          <div className="h-full bg-cyan-400 rounded-full transition-all" style={{ width: `${pct}%` }} />
        </div>
      </div>

      <button onClick={reboot}
        className="w-full px-4 py-2.5 rounded-lg bg-red-900 text-gray-200 font-semibold text-sm">
        {t('reboot')}
      </button>
    </div>
  )
}
