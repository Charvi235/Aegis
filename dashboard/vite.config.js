import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
  },
  build: {
    // recharts + react bundle together; 600 kB is normal for a charting app.
    // Raise the warning threshold so CI doesn't flag it as an error.
    chunkSizeWarningLimit: 650,
    rollupOptions: {
      output: {
        // Split vendor libs into a separate chunk for better caching.
        manualChunks: {
          react:    ['react', 'react-dom'],
          recharts: ['recharts'],
          lucide:   ['lucide-react'],
        },
      },
    },
  },
})
