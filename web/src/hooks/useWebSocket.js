import { useEffect, useRef, useCallback } from 'preact/hooks'
import { signal } from '@preact/signals'

export const wsConnected = signal(false)

export function useWebSocket(onMessage) {
  const wsRef = useRef(null)
  const retryRef = useRef(1000)
  const cbRef = useRef(onMessage)
  cbRef.current = onMessage

  const connect = useCallback(() => {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws'
    const ws = new WebSocket(`${proto}://${location.host}/ws`)
    wsRef.current = ws

    ws.onopen = () => {
      wsConnected.value = true
      retryRef.current = 1000
    }
    ws.onclose = () => {
      wsConnected.value = false
      setTimeout(connect, Math.min(retryRef.current, 10000))
      retryRef.current *= 1.5
    }
    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data)
        cbRef.current?.(msg)
      } catch (e) { /* ignore malformed */ }
    }
  }, [])

  useEffect(() => {
    connect()
    return () => { if (wsRef.current) wsRef.current.close() }
  }, [connect])
}
