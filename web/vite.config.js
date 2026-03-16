import { defineConfig } from 'vite'
import preact from '@preact/preset-vite'
import tailwindcss from '@tailwindcss/vite'
import { resolve } from 'path'
import { writeFileSync, readFileSync, readdirSync, mkdirSync, existsSync } from 'fs'
import { gzipSync } from 'zlib'

// Strip crossorigin attributes — not needed on embedded device, can cause issues
function stripCrossorigin() {
  return {
    name: 'strip-crossorigin',
    transformIndexHtml(html) {
      return html.replace(/\s+crossorigin/g, '')
    },
  }
}

// Post-build plugin: gzip all output files into ../data/
function gzipToData() {
  return {
    name: 'gzip-to-data',
    closeBundle() {
      const distDir = resolve(__dirname, 'dist')
      const dataDir = resolve(__dirname, '..', 'data')
      if (!existsSync(distDir)) return
      if (!existsSync(dataDir)) mkdirSync(dataDir, { recursive: true })

      const files = readdirSync(distDir).filter(f => !f.startsWith('.'))
      for (const file of files) {
        const src = resolve(distDir, file)
        const raw = readFileSync(src)
        const gz = gzipSync(raw, { level: 9 })
        const dest = resolve(dataDir, file + '.gz')
        writeFileSync(dest, gz)
        const pct = ((gz.length / raw.length) * 100).toFixed(0)
        console.log(`  ${file.padEnd(24)} ${raw.length.toString().padStart(6)} -> ${gz.length.toString().padStart(6)}  (${pct}%)`)
      }
      console.log(`\n  ${files.length} files gzipped into ${dataDir}\n`)
    },
  }
}

export default defineConfig({
  plugins: [
    preact(),
    tailwindcss(),
    stripCrossorigin(),
    gzipToData(),
  ],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    modulePreload: false,  // no crossorigin or modulepreload — embedded device
    rollupOptions: {
      input: {
        dashboard: resolve(__dirname, 'index.html'),
        wizard: resolve(__dirname, 'wizard.html'),
      },
      output: {
        // Flat filenames — no nested dirs, no hashes (LittleFS friendly)
        entryFileNames: '[name].js',
        chunkFileNames: '[name].js',
        assetFileNames: '[name][extname]',
      },
    },
  },
  server: {
    proxy: {
      '/api': 'http://192.168.1.87',
      '/ws': { target: 'ws://192.168.1.87', ws: true },
    },
  },
})
