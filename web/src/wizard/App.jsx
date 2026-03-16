import { useState, useEffect, useRef } from 'preact/hooks'
import { L, langs, flags } from './i18n'

function useLang() {
  const stored = localStorage.getItem('lang') || navigator.language.slice(0, 2)
  const [lang, setLang] = useState(langs.includes(stored) ? stored : 'en')
  const t = (k) => L[lang]?.[k] || L.en[k] || k
  const cycle = () => {
    const next = langs[(langs.indexOf(lang) + 1) % langs.length]
    setLang(next)
    localStorage.setItem('lang', next)
  }
  const flag = flags[langs.indexOf(lang)] || flags[0]
  return { lang, t, cycle, flag }
}

function SignalBars({ rssi }) {
  const s = rssi > -50 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1
  return (
    <span className="inline-flex items-end gap-0.5 h-4 align-middle ml-1.5">
      {[1, 2, 3, 4].map(i => (
        <span key={i} className={`w-1 rounded-sm ${i <= s ? 'bg-cyan-400' : 'bg-gray-700'}`}
              style={{ height: `${i * 4}px` }} />
      ))}
    </span>
  )
}

function EyeIcon({ open }) {
  if (open) return (
    <svg viewBox="0 0 141 95" className="w-5.5 h-5.5 opacity-60"><g transform="translate(3.59,3.59)">
      <path d="m14.38,44.48c15.68,38.16 86.18,37.85 101.15,0" fill="none" stroke="#e8e8e8" strokeWidth="7.9" strokeLinecap="round" strokeLinejoin="round"/>
      <path d="m14.38,44.54c15.68,-38.16 86.18,-37.85 101.15,0" fill="none" stroke="#e8e8e8" strokeWidth="7.9" strokeLinecap="round" strokeLinejoin="round"/>
      <path d="M79.57,45.37C80.67,59.16 58.67,65.23 52.82,52.46 46.23,40.9 59.52,25.67 71.71,32.65" fill="none" stroke="#e8e8e8" strokeWidth="7.9" strokeLinecap="round" strokeLinejoin="round"/>
    </g></svg>
  )
  return (
    <svg viewBox="0 0 141 95" className="w-5.5 h-5.5 opacity-60"><g transform="translate(3.59,3.59)">
      <path d="m14.38,19.3c15.18,45.66 85.72,44.75 101.15,0" fill="none" stroke="#e8e8e8" strokeWidth="7.9" strokeLinecap="round" strokeLinejoin="round"/>
      <path d="M30.24,42.48 19.8,59.04" fill="none" stroke="#e8e8e8" strokeWidth="7.9" strokeLinecap="round"/>
      <path d="M99,42.48 109.44,59.04" fill="none" stroke="#e8e8e8" strokeWidth="7.9" strokeLinecap="round"/>
      <path d="m77.23,51.99 4.3,19.1" fill="none" stroke="#e8e8e8" strokeWidth="7.9" strokeLinecap="round"/>
      <path d="M52.37,51.99 48.07,71.09" fill="none" stroke="#e8e8e8" strokeWidth="7.9" strokeLinecap="round"/>
    </g></svg>
  )
}

export default function WizardApp() {
  const { t, cycle, flag } = useLang()
  const [networks, setNetworks] = useState(null)
  const [selected, setSelected] = useState('')
  const [manualSsid, setManualSsid] = useState('')
  const [password, setPassword] = useState('')
  const [showPw, setShowPw] = useState(false)
  const pwRef = useRef(null)
  const [status, setStatus] = useState(null) // null | 'connecting' | 'ok' | 'err'
  const [errMsg, setErrMsg] = useState('')
  const timerRef = useRef(null)

  useEffect(() => {
    fetch('/api/scan').then(r => r.json()).then(d => {
      setNetworks(d)
      if (d.length) setSelected(d[0].ssid)
    }).catch(() => setNetworks([]))
    return () => clearTimeout(timerRef.current)
  }, [])

  const ssid = selected === '__other__' ? manualSsid : selected

  function pollStatus() {
    fetch('/api/status').then(r => r.json()).then(d => {
      if (d.status === 'connected') setStatus('ok')
      else if (d.status === 'failed') { setStatus('err'); setErrMsg('connection failed') }
      else if (d.status === 'connecting') timerRef.current = setTimeout(pollStatus, 1000)
    }).catch(() => { timerRef.current = setTimeout(pollStatus, 1500) })
  }

  function onSubmit(e) {
    e.preventDefault()
    if (!ssid) return
    setStatus('connecting')
    fetch('/api/wifi', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ssid, pass: password }),
    }).then(r => r.json()).then(d => {
      if (d.ok) timerRef.current = setTimeout(pollStatus, 1000)
      else { setStatus('err'); setErrMsg(d.error || 'unknown') }
    }).catch(() => { setStatus('err'); setErrMsg(t('netErr')) })
  }

  const busy = status === 'connecting'

  return (
    <div className="min-h-screen bg-gradient-to-b from-[#0d0d1a] to-[#1a1a2e] text-gray-200 flex justify-center items-start p-8 px-4">
      <button onClick={cycle} title="Language"
        className="fixed top-3 right-3 w-10 h-10 rounded-full border border-gray-700 bg-[#0d0d1a] text-lg cursor-pointer flex items-center justify-center z-10">
        {flag}
      </button>

      <div className="w-full max-w-[420px]">
        <div className="text-center text-4xl mb-2" style={{ filter: 'drop-shadow(0 0 8px rgba(0,212,255,.4))' }}>🐾</div>
        <h1 className="text-cyan-400 text-2xl font-semibold text-center mb-0.5">{t('title')}</h1>
        <p className="text-center text-gray-500 text-sm mb-6">{t('desc')}</p>

        <form onSubmit={onSubmit}>
          <div className="mb-4">
            <label className="block text-sm text-gray-400 mb-1">{t('network')}</label>
            <select value={selected} onChange={e => setSelected(e.target.value)}
              className="w-full p-3 rounded-lg border border-gray-700 bg-[#0d0d1a] text-gray-200 outline-none focus:border-cyan-400 appearance-none">
              {networks === null && <option value="">{t('scanning')}</option>}
              {networks?.length === 0 && <option value="">{t('noNetworks')}</option>}
              {networks?.map(n => (
                <option key={n.ssid} value={n.ssid}>
                  {n.ssid}{n.auth > 0 ? ' 🔒' : ''}{n.rssi > -50 ? ' ||||' : n.rssi > -65 ? ' |||' : n.rssi > -75 ? ' ||' : ' |'}
                </option>
              ))}
              <option value="__other__">{t('other')}</option>
            </select>
            {selected === '__other__' && (
              <input type="text" value={manualSsid} onInput={e => setManualSsid(e.target.value)}
                placeholder="SSID" maxLength={32}
                className="w-full p-3 mt-2 rounded-lg border border-gray-700 bg-[#0d0d1a] text-gray-200 outline-none focus:border-cyan-400" />
            )}
          </div>

          <div className="mb-4">
            <label className="block text-sm text-gray-400 mb-1">{t('password')}</label>
            <div className="relative">
              <input ref={pwRef} type={showPw ? 'text' : 'password'} value={password}
                onInput={e => setPassword(e.target.value)} autocomplete="off"
                className="w-full p-3 pr-11 rounded-lg border border-gray-700 bg-[#0d0d1a] text-gray-200 outline-none focus:border-cyan-400" />
              <button type="button" onClick={() => { setShowPw(!showPw); setTimeout(() => { const el = pwRef.current; if (el) { el.focus(); el.selectionStart = el.selectionEnd = el.value.length } }, 0) }}
                className="absolute right-1 top-1/2 -translate-y-1/2 w-9 h-9 border-none bg-transparent cursor-pointer flex items-center justify-center rounded-md">
                <EyeIcon open={showPw} />
              </button>
            </div>
          </div>

          <button type="submit" disabled={busy || !ssid}
            className="w-full p-3.5 rounded-lg border-none bg-cyan-400 text-[#0d0d1a] font-semibold cursor-pointer disabled:opacity-40 disabled:cursor-default active:opacity-80 min-h-[44px]">
            {t('connect')}
          </button>
        </form>

        {status === 'connecting' && (
          <div className="mt-4 p-2.5 rounded-lg text-sm bg-[#3a3a1b] border border-[#6a6a2d]">{t('connecting')}</div>
        )}
        {status === 'ok' && (
          <div className="mt-4 p-2.5 rounded-lg text-sm bg-[#1b3a2a] border border-[#2d6a3e]">{t('success')}</div>
        )}
        {status === 'err' && (
          <div className="mt-4 p-2.5 rounded-lg text-sm bg-[#3a1b1b] border border-[#6a2d2d]">{t('fail')}{errMsg}</div>
        )}
      </div>
    </div>
  )
}
