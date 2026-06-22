import { defineConfig } from 'vite';

export default defineConfig({
  server: {
    host: 'localhost',
    port: 8443,
  },
  optimizeDeps: {
    exclude: ['@liamlangli/ui'],
  },
});
