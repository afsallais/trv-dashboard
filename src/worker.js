import { onRequest } from './ewelink-api.js';

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);
    if (pathname.startsWith('/api/')) return onRequest({ request, env });
    return env.ASSETS.fetch(request);
  },
};
