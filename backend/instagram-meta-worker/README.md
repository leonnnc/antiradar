# CYBERDECK Insta Monitor Backend

Backend minimo para que `INSTA MONITOR` muestre datos reales usando Meta Graph API.

El ESP32 no debe guardar tokens de Meta. Este worker recibe:

```txt
/ig?user=usuario
```

Y responde:

```json
{
  "status": "ok",
  "username": "usuario",
  "followers": 12345,
  "followers_count": 12345,
  "media": 100,
  "media_count": 100,
  "source": "meta_graph_business_discovery"
}
```

## Requisitos

- Cuenta Instagram profesional, Business o Creator.
- Cuenta vinculada a una pagina de Facebook.
- App en Meta for Developers con permisos necesarios para Instagram Graph API.
- Access token largo del lado servidor.
- `IG_USER_ID`: el ID de una cuenta profesional autorizada que pueda hacer Business Discovery.

## Deploy con Cloudflare Workers

1. Copia el archivo de ejemplo:

```powershell
Copy-Item wrangler.toml.example wrangler.toml
```

2. Instala dependencias:

```powershell
npm install
```

3. Guarda secretos:

```powershell
npx wrangler secret put META_ACCESS_TOKEN
npx wrangler secret put IG_USER_ID
```

4. Despliega:

```powershell
npx wrangler deploy
```

5. Prueba:

```txt
https://TU-WORKER.workers.dev/ig?user=pepeangelll
```

## Firmware

Cuando tengas la URL del worker, compila el firmware agregando:

```ini
-D IG_API_URL=\"https://TU-WORKER.workers.dev/ig?user={user}\"
```

El firmware reemplaza `{user}` por el usuario escrito en el teclado.

## Nota importante

Business Discovery no es scraping. Es la ruta correcta para datos reales cuando la cuenta y permisos estan autorizados por Meta. Si `IG_API_URL` no esta configurado, el firmware solo muestra `DEMO sin API`, que no son datos reales.
