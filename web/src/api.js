export function api(path, opts) {
  return fetch(path, opts).then(r => {
    if (!r.ok) throw new Error(r.status)
    return r.json()
  })
}
