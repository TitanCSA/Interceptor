# Interceptor JavaScript / TypeScript SDK

`npm install interceptor-sdk`

Node.js 20+ Windows x64. Bundled Node-API v8 addon and product native DLL; no compiler or Python required to install. Driver installation and original product licensing remain separate. Supported families: keyboard, mouse, hid, xinput, xinputhid, gip.

```js
const { Interceptor } = require("interceptor-sdk");
const client = new Interceptor();
try { console.log(client.info()); } finally { client.close(); }
```

[API documentation](docs/API.md). Construction does not start capture.
