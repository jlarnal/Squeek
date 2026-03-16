import { useState, useEffect, useRef, useCallback } from 'preact/hooks'
import { api } from '../api'
import { t } from '../i18n'

const FOCAL = 1200

function deg(a) { return a * Math.PI / 180 }

function rotatePoint(x, y, z, rx, ry) {
  const cy = Math.cos(deg(ry)), sy = Math.sin(deg(ry))
  const cx = Math.cos(deg(rx)), sx = Math.sin(deg(rx))
  const x1 = cy * x + sy * z, z1 = -sy * x + cy * z
  const y1 = cx * y - sx * z1, z2 = sx * y + cx * z1
  return [x1, y1, z2]
}

function project(x, y, z, W, H, zoom) {
  const s = FOCAL / (FOCAL + z)
  return [W / 2 + x * s * zoom, H / 2 + y * s * zoom, s]
}

function hasPositions(peers) {
  return peers.length >= 2 && peers.some(p => p.pos[0] !== 0 || p.pos[1] !== 0 || p.pos[2] !== 0)
}

function NodeInfo({ peer, onClose }) {
  if (!peer) return null
  const fl = []
  if (peer.flags & 0x01) fl.push('alive')
  if (peer.flags & 0x02) fl.push('sleeping')
  if (peer.flags & 0x04) fl.push('dead')
  return (
    <div className="absolute bottom-2 left-2 right-2 bg-[rgba(13,13,26,0.9)] border border-gray-700 rounded-lg p-2.5 text-xs"
         onClick={e => e.stopPropagation()}>
      <b>{peer.mac}</b>{peer.is_gateway ? ' (GW)' : ''}<br />
      {t('battery')}: {peer.battery_mv} mV<br />
      Pos: [{peer.pos.map(v => v.toFixed(1)).join(', ')}]<br />
      Status: {fl.join(', ')}
    </div>
  )
}

function ScanOverlay({ peers, scanActive, onScan }) {
  const hasPos = hasPositions(peers)
  if (hasPos) return null

  const needMore = peers.length < 2
  const label = needMore ? t('needNodes') : scanActive ? t('scanning') : t('buildMap')
  const disabled = needMore || scanActive

  return (
    <div className="absolute inset-0 flex flex-col items-center justify-center bg-[rgba(13,13,26,0.85)] rounded-lg" style={{ zIndex: 10 }}>
      <div className="relative w-1/2 max-w-45 pointer-events-none" style={{ aspectRatio: '1' }}>
        {scanActive ? (
          <>
            {/* Scanning: radar background + rotating sector ("hourglass") */}
            <svg viewBox="0 0 84.67 84.67" xmlns="http://www.w3.org/2000/svg" className="absolute inset-0 w-full h-full">
              <circle cx="42.33" cy="42.33" r="34" fill="none" stroke="#949494" strokeWidth="2.1" strokeLinecap="round" />
              <circle cx="42.33" cy="42.33" r="24" fill="none" stroke="#949494" strokeWidth="2.1" strokeLinecap="round" />
              <circle cx="42.33" cy="42.33" r="14" fill="none" stroke="#949494" strokeWidth="2.1" strokeLinecap="round" />
              {[
                { x: 73.23, y: 56.94 },
                { x: 60.90, y: 13.64 },
                { x: 26.44, y: 23.77 },
                { x: 18.40, y: 44.07 },
                { x: 34.08, y: 52.98 },
              ].map((n, i) => (
                <line key={i} x1="42.33" y1="42.33" x2={n.x} y2={n.y}
                  style={{
                    stroke: '#4f3',
                    strokeWidth: 1.5,
                    strokeLinecap: 'round',
                    strokeDasharray: '1 8',
                    fill: 'none',
                    animation: 'dash-flow 5s linear infinite',
                  }} />
              ))}
              <circle cx="42.33" cy="42.33" r="3.96" fill="#eee" />
              {[
                { cx: 60.90, cy: 13.64, delay: 2937 },
                { cx: 73.23, cy: 56.94, delay: 624 },
                { cx: 34.08, cy: 52.98, delay: 1478 },
                { cx: 18.40, cy: 44.07, delay: 1878 },
                { cx: 26.44, cy: 23.77, delay: 2324 },
              ].map((n, i) => (
                <circle key={i} cx={n.cx} cy={n.cy} r="3.96" fill="#e3e3e3"
                  style={{
                    transformOrigin: `${n.cx}px ${n.cy}px`,
                    animation: `dot-pulse 3s ease-out ${n.delay}ms infinite`,
                  }} />
              ))}
            </svg>
            <svg viewBox="0 0 84.67 84.67" xmlns="http://www.w3.org/2000/svg"
                 xmlnsXlink="http://www.w3.org/1999/xlink"
                 className="absolute inset-0 w-full h-full"
                 style={{ animation: 'scan-spin 3s linear infinite' }}>
              <defs>
                <linearGradient id="lg1">
                  <stop offset="0" style={{ stopColor: '#ffffff', stopOpacity: 1 }} />
                  <stop offset="1" style={{ stopColor: '#ffffff', stopOpacity: 0 }} />
                </linearGradient>
                <linearGradient xlinkHref="#lg1" id="lg2"
                  x1="61.739094" y1="21.596378" x2="37.606518" y2="23.70332"
                  gradientUnits="userSpaceOnUse" />
              </defs>
              <path style={{ fill: 'url(#lg2)', fillOpacity: 1 }}
                    d="M 32.879705,5.0733045 C 38.930661,3.5380538 45.165966,3.5199896 51.074831,4.8998321 56.983696,6.2796746 62.566121,9.0574239 67.311316,13.1138 L 42.333334,42.333333 Z" />
            </svg>
          </>
        ) : (
          /* Unscanned: radar rings + animated question marks popping one at a time */
          <svg viewBox="0 0 84.67 84.67" xmlns="http://www.w3.org/2000/svg" className="absolute inset-0 w-full h-full">
            <style>{`
              @keyframes qmark-pop {
                0%   { opacity: 0; transform: scale(0); }
                2%   { opacity: 1; transform: scale(1.15); }
                5%   { opacity: 1; transform: scale(1); }
                14%  { opacity: 0; transform: scale(0); }
                100% { opacity: 0; transform: scale(0); }
              }
              .qm { transform-box: fill-box; transform-origin: center; opacity: 0;
                     animation: qmark-pop 12s ease-out infinite; }
            `}</style>
            <circle cx="42.33" cy="42.33" r="34" fill="none" stroke="#949494" strokeWidth="2.1" strokeLinecap="round" />
            <circle cx="42.33" cy="42.33" r="24" fill="none" stroke="#949494" strokeWidth="2.1" strokeLinecap="round" />
            <circle cx="42.33" cy="42.33" r="14" fill="none" stroke="#949494" strokeWidth="2.1" strokeLinecap="round" />
            <circle cx="42.33" cy="42.33" r="3.96" fill="none" stroke="#949494" strokeWidth="2.1" />
            {/* 6 question marks — staggered delays jump around the radar for a random feel */}
            <path className="qm" style={{ animationDelay: '0s' }} fill="#fff"
              d="m 33.717016,33.778089 c 0.09178,-3.385303 2.994305,-6.150828 6.170078,-6.881555 3.277917,-0.808188 7.147677,-0.167787 9.494624,2.408839 1.845371,2.08958 2.168839,5.521895 0.318559,7.715491 -1.406139,1.895302 -3.549807,3.085108 -4.96203,4.965579 -0.899327,1.100021 -0.368368,2.888101 -1.711699,3.685905 -1.299036,0.834842 -3.174847,-0.169639 -3.215908,-1.718489 -0.184012,-2.143844 0.884163,-4.227292 2.51048,-5.580886 1.324165,-1.408034 3.309221,-2.412043 3.845403,-4.398501 0.367809,-1.917268 -1.374947,-3.753079 -3.273381,-3.75275 -1.487786,-0.163256 -3.137147,0.442899 -3.82803,1.844588 -0.779305,1.163699 -0.761019,2.891287 -2.083624,3.650485 -1.362585,0.713034 -3.280484,-0.369939 -3.264472,-1.938706 z m 8.406011,18.879074 c -1.530292,0.105538 -2.863536,-1.368927 -2.616389,-2.878382 0.151603,-1.762645 2.321654,-2.856856 3.848067,-1.98778 1.620286,0.834954 1.851085,3.459008 0.236459,4.429508 -0.432942,0.283516 -0.94948,0.443537 -1.468137,0.436654 z" />
            <path className="qm" style={{ animationDelay: '2s' }} fill="#fff"
              d="m 44.993615,13.948237 c 0.09178,-3.385303 2.994305,-6.1508274 6.170078,-6.8815548 3.277917,-0.8081882 7.147677,-0.1677868 9.494624,2.4088392 1.845371,2.0895796 2.168839,5.5218946 0.318559,7.7154906 -1.406139,1.895302 -3.549807,3.085108 -4.96203,4.965579 -0.899327,1.100021 -0.368368,2.888101 -1.711699,3.685905 -1.299036,0.834842 -3.174847,-0.169639 -3.215908,-1.718489 -0.184012,-2.143844 0.884163,-4.227292 2.51048,-5.580886 1.324165,-1.408034 3.309221,-2.412043 3.845403,-4.398501 0.367809,-1.917268 -1.374947,-3.753079 -3.273381,-3.75275 -1.487786,-0.163256 -3.137147,0.442899 -3.82803,1.844588 -0.779305,1.163699 -0.761019,2.891287 -2.083624,3.650485 -1.362585,0.713034 -3.280484,-0.369939 -3.264472,-1.938706 z m 8.406011,18.879074 c -1.530292,0.105538 -2.863536,-1.368927 -2.616389,-2.878382 0.151603,-1.762645 2.321654,-2.856856 3.848067,-1.98778 1.620286,0.834954 1.851085,3.459008 0.236459,4.429508 -0.432942,0.283516 -0.94948,0.443537 -1.468137,0.436654 z" />
            <path className="qm" style={{ animationDelay: '4s' }} fill="#fff"
              d="m 8.2869658,56.067773 c 0.09178,-3.385303 2.9943052,-6.150827 6.1700782,-6.881555 3.277917,-0.808188 7.147677,-0.167787 9.494624,2.408839 1.845371,2.08958 2.168839,5.521895 0.318559,7.715491 -1.406139,1.895302 -3.549807,3.085108 -4.96203,4.965579 -0.899327,1.100021 -0.368368,2.888101 -1.711699,3.685905 -1.299036,0.834842 -3.174847,-0.169639 -3.215908,-1.718489 -0.184012,-2.143844 0.884163,-4.227292 2.51048,-5.580886 1.324165,-1.408034 3.309221,-2.412043 3.845403,-4.398501 0.367809,-1.917268 -1.374947,-3.753079 -3.273381,-3.75275 -1.487786,-0.163256 -3.137147,0.442899 -3.82803,1.844588 -0.779305,1.163699 -0.761019,2.891287 -2.083624,3.650485 -1.362585,0.713034 -3.2804842,-0.369939 -3.2644722,-1.938706 z m 8.4060112,18.879074 c -1.530292,0.105538 -2.863536,-1.368927 -2.616389,-2.878382 0.151603,-1.762645 2.321654,-2.856856 3.848067,-1.98778 1.620286,0.834954 1.851085,3.459008 0.236459,4.429508 -0.432942,0.283516 -0.94948,0.443537 -1.468137,0.436654 z" />
            <path className="qm" style={{ animationDelay: '6s' }} fill="#fff"
              d="m 57.535161,45.889699 c 0.09178,-3.385303 2.994305,-6.150828 6.170078,-6.881555 3.277917,-0.808188 7.147677,-0.167787 9.494624,2.408839 1.845371,2.08958 2.168839,5.521895 0.318559,7.715491 -1.406139,1.895302 -3.549807,3.085108 -4.96203,4.965579 -0.899327,1.100021 -0.368368,2.888101 -1.711699,3.685905 -1.299036,0.834842 -3.174847,-0.169639 -3.215908,-1.718489 -0.184012,-2.143844 0.884163,-4.227292 2.51048,-5.580886 1.324165,-1.408034 3.309221,-2.412043 3.845403,-4.398501 0.367809,-1.917268 -1.374947,-3.753079 -3.273381,-3.75275 -1.487786,-0.163256 -3.137147,0.442899 -3.82803,1.844588 -0.779305,1.163699 -0.761019,2.891287 -2.083624,3.650485 -1.362585,0.713034 -3.280484,-0.369939 -3.264472,-1.938706 z m 8.406011,18.879074 c -1.530292,0.105538 -2.863536,-1.368927 -2.616389,-2.878382 0.151603,-1.762645 2.321654,-2.856856 3.848067,-1.98778 1.620286,0.834954 1.851085,3.459008 0.236459,4.429508 -0.432942,0.283516 -0.94948,0.443537 -1.468137,0.436654 z" />
            <path className="qm" style={{ animationDelay: '8s' }} fill="#fff"
              d="m 40.499201,61.932324 c 0.09178,-3.385303 2.994305,-6.150828 6.170078,-6.881555 3.277917,-0.808188 7.147677,-0.167787 9.494624,2.408839 1.845371,2.08958 2.168839,5.521895 0.318559,7.715491 -1.406139,1.895302 -3.549807,3.085108 -4.96203,4.965579 -0.899327,1.100021 -0.368368,2.888101 -1.711699,3.685905 -1.299036,0.834842 -3.174847,-0.169639 -3.215908,-1.718489 -0.184012,-2.143844 0.884163,-4.227292 2.51048,-5.580886 1.324165,-1.408034 3.309221,-2.412043 3.845403,-4.398501 0.367809,-1.917268 -1.374947,-3.753079 -3.273381,-3.75275 -1.487786,-0.163256 -3.137147,0.442899 -3.82803,1.844588 -0.779305,1.163699 -0.761019,2.891287 -2.083624,3.650485 -1.362585,0.713034 -3.280484,-0.369939 -3.264472,-1.938706 z m 8.406011,18.879074 c -1.530292,0.105538 -2.863536,-1.368927 -2.616389,-2.878382 0.151603,-1.762645 2.321654,-2.856856 3.848067,-1.98778 1.620286,0.834954 1.851085,3.459008 0.236459,4.429508 -0.432942,0.283516 -0.94948,0.443537 -1.468137,0.436654 z" />
            <path className="qm" style={{ animationDelay: '10s' }} fill="#fff"
              d="m 16.316425,16.043531 c 0.09178,-3.385303 2.994305,-6.1508278 6.170078,-6.8815552 3.277917,-0.8081882 7.147677,-0.1677868 9.494624,2.4088392 1.845371,2.08958 2.168839,5.521895 0.318559,7.715491 -1.406139,1.895302 -3.549807,3.085108 -4.96203,4.965579 -0.899327,1.100021 -0.368368,2.888101 -1.711699,3.685905 -1.299036,0.834842 -3.174847,-0.169639 -3.215908,-1.718489 -0.184012,-2.143844 0.884163,-4.227292 2.51048,-5.580886 1.324165,-1.408034 3.309221,-2.412043 3.845403,-4.398501 0.367809,-1.917268 -1.374947,-3.753079 -3.273381,-3.75275 -1.487786,-0.163256 -3.137147,0.442899 -3.82803,1.844588 -0.779305,1.163699 -0.761019,2.891287 -2.083624,3.650485 -1.362585,0.713034 -3.280484,-0.369939 -3.264472,-1.938706 z m 8.406011,18.879074 c -1.530292,0.105538 -2.863536,-1.368927 -2.616389,-2.878382 0.151603,-1.762645 2.321654,-2.856856 3.848067,-1.98778 1.620286,0.834954 1.851085,3.459008 0.236459,4.429508 -0.432942,0.283516 -0.94948,0.443537 -1.468137,0.436654 z" />
          </svg>
        )}
      </div>
      <span className="text-gray-500 mt-3 text-sm pointer-events-none">{label}</span>
      <button className="mt-2 px-4 py-1.5 rounded-lg bg-cyan-400 text-gray-950 text-sm font-semibold disabled:opacity-40 disabled:cursor-default"
              disabled={disabled} onClick={onScan}
              onPointerDown={e => e.stopPropagation()}>
        {label}
      </button>
    </div>
  )
}

export default function MapTab({ peers, distances, status, onRefresh, toast }) {
  const canvasRef = useRef(null)
  const vpRef = useRef(null)
  const [view, setView] = useState({ rx: -20, ry: 30, zoom: 1 })
  const [selected, setSelected] = useState(null)
  const [scanActive, setScanActive] = useState(false)
  const dragRef = useRef({ dragging: false, pinching: false, sx: 0, sy: 0, sd: 0, sz: 1 })

  useEffect(() => { onRefresh() }, [])

  const renderCanvas = useCallback(() => {
    const canvas = canvasRef.current
    const vp = vpRef.current
    if (!canvas || !vp) return
    const ctx = canvas.getContext('2d')
    const W = vp.clientWidth, H = vp.clientHeight
    const dpr = window.devicePixelRatio || 1
    canvas.width = W * dpr; canvas.height = H * dpr
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
    ctx.clearRect(0, 0, W, H)

    if (!hasPositions(peers)) return

    // Center + scale
    const ctr = [0, 0, 0]
    peers.forEach(p => { ctr[0] += p.pos[0]; ctr[1] += p.pos[1]; ctr[2] += p.pos[2] })
    ctr[0] /= peers.length; ctr[1] /= peers.length; ctr[2] /= peers.length

    let maxR = 0
    peers.forEach(p => {
      const dx = p.pos[0] - ctr[0], dy = p.pos[1] - ctr[1], dz = p.pos[2] - ctr[2]
      maxR = Math.max(maxR, Math.sqrt(dx * dx + dy * dy + dz * dz))
    })
    const ws = maxR > 0 ? Math.min(W, H) * 0.3 / maxR : 1

    // Project all peers
    const projected = peers.map(p => {
      const wx = (p.pos[0] - ctr[0]) * ws, wy = (p.pos[1] - ctr[1]) * ws, wz = (p.pos[2] - ctr[2]) * ws
      const r = rotatePoint(wx, wy, wz, view.rx, view.ry)
      const pr = project(r[0], r[1], r[2], W, H, view.zoom)
      return { peer: p, sx: pr[0], sy: pr[1], s: pr[2], rz: r[2] }
    })

    // Draw edges
    const edges = []
    distances.forEach(e => {
      const pa = projected.find(n => n.peer.idx === e.a)
      const pb = projected.find(n => n.peer.idx === e.b)
      if (pa && pb) edges.push({ a: pa, b: pb, d: e.d, avgZ: (pa.rz + pb.rz) / 2 })
    })
    edges.sort((a, b) => a.avgZ - b.avgZ)

    edges.forEach(e => {
      const alpha = 0.1 + 0.15 * Math.min(e.a.s, e.b.s)
      ctx.strokeStyle = `rgba(255,255,255,${alpha.toFixed(2)})`
      ctx.lineWidth = Math.max(0.5, 1 * Math.min(e.a.s, e.b.s))
      ctx.beginPath(); ctx.moveTo(e.a.sx, e.a.sy); ctx.lineTo(e.b.sx, e.b.sy); ctx.stroke()
      // Distance label
      const mx = (e.a.sx + e.b.sx) / 2, my = (e.a.sy + e.b.sy) / 2
      const la = 0.3 + 0.4 * Math.min(e.a.s, e.b.s)
      const ls = Math.max(8, 10 * Math.min(e.a.s, e.b.s))
      ctx.font = `${ls.toFixed(0)}px system-ui`
      ctx.fillStyle = `rgba(136,136,136,${la.toFixed(2)})`
      ctx.textAlign = 'center'; ctx.textBaseline = 'bottom'
      ctx.fillText(`${Math.round(e.d)}cm`, mx, my - 2)
    })

    // Light direction: fixed in world space, rotated into screen space
    const [lsx, lsy] = rotatePoint(-1, -1, -1, view.rx, view.ry)
    const lmag = Math.sqrt(lsx * lsx + lsy * lsy) || 1
    const lnx = lsx / lmag, lny = lsy / lmag

    // Draw nodes (sorted back-to-front)
    projected.sort((a, b) => a.rz - b.rz)
    projected.forEach(n => {
      const p = n.peer
      const baseSize = p.is_gateway ? 18 : 14
      const size = Math.max(6, baseSize * n.s)
      const alpha = 0.4 + 0.6 * n.s
      const rad = size / 2

      // Sphere via radial gradient: highlight tracks world-space light
      const base = p.is_gateway ? [0, 149, 182] : [50, 205, 50]  // #0095b6 / #32cd32
      const grad = ctx.createRadialGradient(
        n.sx + lnx * rad * 0.35, n.sy + lny * rad * 0.35, rad * 0.05,
        n.sx, n.sy, rad
      )
      grad.addColorStop(0, `rgba(${Math.min(base[0]+140,255)},${Math.min(base[1]+80,255)},${Math.min(base[2]+80,255)},${alpha})`)
      grad.addColorStop(0.45, `rgba(${base[0]},${base[1]},${base[2]},${alpha})`)
      grad.addColorStop(1, `rgba(${base[0]>>1},${base[1]>>1},${base[2]>>1},${alpha * 0.9})`)

      ctx.beginPath()
      ctx.arc(n.sx, n.sy, rad, 0, Math.PI * 2)
      ctx.fillStyle = grad
      ctx.fill()

      // Label
      ctx.font = `${Math.max(8, 9 * n.s).toFixed(0)}px system-ui`
      ctx.fillStyle = `rgba(224,224,224,${alpha.toFixed(2)})`
      ctx.textAlign = 'center'; ctx.textBaseline = 'top'
      ctx.fillText(p.is_gateway ? 'GW' : `#${p.idx}`, n.sx, n.sy + rad + 2)
    })
  }, [peers, distances, view])

  useEffect(() => { renderCanvas() }, [renderCanvas])

  // Pointer drag (mouse)
  function onPointerDown(e) {
    if (e.pointerType === 'touch') return
    dragRef.current.dragging = true
    dragRef.current.sx = e.clientX; dragRef.current.sy = e.clientY
    vpRef.current?.setPointerCapture(e.pointerId)
  }
  function onPointerMove(e) {
    if (!dragRef.current.dragging || e.pointerType === 'touch') return
    setView(v => ({
      ...v,
      ry: v.ry + (e.clientX - dragRef.current.sx) * 0.5,
      rx: Math.max(-80, Math.min(80, v.rx - (e.clientY - dragRef.current.sy) * 0.5)),
    }))
    dragRef.current.sx = e.clientX; dragRef.current.sy = e.clientY
  }
  function onPointerUp() { dragRef.current.dragging = false }

  // Touch drag + pinch
  function getTouchDist(t) {
    const dx = t[0].clientX - t[1].clientX, dy = t[0].clientY - t[1].clientY
    return Math.sqrt(dx * dx + dy * dy)
  }
  function onTouchStart(e) {
    if (e.touches.length === 1) {
      dragRef.current = { ...dragRef.current, dragging: true, pinching: false, sx: e.touches[0].clientX, sy: e.touches[0].clientY }
    }
    if (e.touches.length === 2) {
      dragRef.current = { ...dragRef.current, pinching: true, dragging: false, sd: getTouchDist(e.touches), sz: view.zoom }
    }
  }
  function onTouchMove(e) {
    const d = dragRef.current
    if (d.pinching && e.touches.length === 2) {
      const dist = getTouchDist(e.touches)
      setView(v => ({ ...v, zoom: Math.max(0.3, Math.min(4, d.sz * (dist / d.sd))) }))
    } else if (d.dragging && e.touches.length === 1) {
      setView(v => ({
        ...v,
        ry: v.ry + (e.touches[0].clientX - d.sx) * 0.5,
        rx: Math.max(-80, Math.min(80, v.rx - (e.touches[0].clientY - d.sy) * 0.5)),
      }))
      dragRef.current.sx = e.touches[0].clientX; dragRef.current.sy = e.touches[0].clientY
    }
  }
  function onTouchEnd() { dragRef.current.dragging = false; dragRef.current.pinching = false }

  function onWheel(e) {
    if (!hasPositions(peers)) return
    e.preventDefault()
    setView(v => ({ ...v, zoom: Math.max(0.3, Math.min(4, v.zoom * (1 - e.deltaY * 0.001))) }))
  }

  function onScan() {
    console.log('[scan] onScan called, scanActive=', scanActive)
    if (scanActive) return
    setScanActive(true)
    console.log('[scan] POST /api/sweep')
    api('/api/sweep', { method: 'POST' })
      .then(() => { console.log('[scan] sweep OK'); toast(t('scanning')) })
      .catch(err => { console.error('[scan] sweep failed', err); setScanActive(false) })
  }

  function resetView() { setView({ rx: -20, ry: 30, zoom: 1 }) }

  return (
    <>
      <div ref={vpRef} className="w-full overflow-hidden rounded-lg relative"
           style={{ touchAction: 'none', background: 'radial-gradient(circle at center, #151530, #0d0d1a)',
                    height: 'calc(100dvh - 10.5rem)' }}
           onPointerDown={onPointerDown} onPointerMove={onPointerMove} onPointerUp={onPointerUp}
           onTouchStart={onTouchStart} onTouchMove={onTouchMove} onTouchEnd={onTouchEnd}
           onWheel={onWheel}
           onClick={() => setSelected(null)}>
        <canvas ref={canvasRef} className="absolute inset-0 w-full h-full" />
        <ScanOverlay peers={peers} scanActive={scanActive} onScan={onScan} />
        <div className="absolute top-2 right-2 flex flex-col gap-1">
          <button onClick={resetView} title="Reset view"
                  className="w-8 h-8 rounded-lg border border-gray-700 bg-[rgba(13,13,26,0.8)] text-gray-200 text-base cursor-pointer flex items-center justify-center">
            &#x21bb;
          </button>
        </div>
        {selected && <NodeInfo peer={selected} />}
      </div>
      <div className="fixed bottom-14 left-3 right-3 bg-[#1a1a2e] border border-gray-700 rounded-lg p-3 flex justify-between items-center text-sm z-10">
        <span>{status?.alive_peers ?? 0} {t('peers')}</span>
        <span>{status?.battery_mv ?? '--'} mV</span>
        <span>{status?.dimension ?? '--'}D</span>
      </div>
    </>
  )
}
