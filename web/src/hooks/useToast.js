import { useState, useRef, useCallback } from 'preact/hooks'

export function useToast() {
  const [msg, setMsg] = useState(null)
  const timer = useRef(null)

  const toast = useCallback((text) => {
    setMsg(text)
    clearTimeout(timer.current)
    timer.current = setTimeout(() => setMsg(null), 2000)
  }, [])

  return { msg, toast }
}
