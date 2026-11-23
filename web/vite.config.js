// SPDX-License-Identifier: MIT
import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import tailwindcss from '@tailwindcss/vite';

// The built UI is embedded in the payload and served from the console on
// port 19090. During development the same port is used by Vite, which proxies
// /api to the console. Set PS5_URL when the console is not reachable as "ps5".
export default defineConfig({
  plugins: [svelte(), tailwindcss()],
  server: {
    port: 19090,
    host: true,
    proxy: {
      '/api': process.env.PS5_URL || 'http://ps5:19090',
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
});
