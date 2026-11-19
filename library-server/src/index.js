// Carve DAW's sound library: the files in the carve-library R2 bucket, read
// only, under /v1/. See ../README.md for what is in the bucket and
// ../../scripts/package-library.py for how it gets there.
//
// GET and HEAD only. Range requests are answered, so a client can resume, and
// an ETag is sent so a repeat fetch of library.json can be a 304.

const ALLOWED_PREFIX = "v1/";

export default {
  async fetch(request, env) {
    if (request.method !== "GET" && request.method !== "HEAD")
      return new Response("Method Not Allowed", { status: 405, headers: { Allow: "GET, HEAD" } });

    const url = new URL(request.url);
    const key = decodeURIComponent(url.pathname.slice(1));

    if (key === "" || key === "v1" || key === "v1/")
      return Response.redirect(new URL("/v1/library.json", url).toString(), 302);

    if (!key.startsWith(ALLOWED_PREFIX) || key.includes(".."))
      return new Response("Not Found", { status: 404 });

    const object = request.method === "HEAD"
      ? await env.LIBRARY.head(key)
      : await env.LIBRARY.get(key, { range: request.headers, onlyIf: request.headers });

    if (object === null)
      return new Response("Not Found", { status: 404 });

    const headers = new Headers();
    object.writeHttpMetadata(headers);
    headers.set("ETag", object.httpEtag);
    headers.set("Accept-Ranges", "bytes");
    headers.set("Access-Control-Allow-Origin", "*");

    if (!headers.has("Cache-Control"))
      headers.set("Cache-Control", key.endsWith(".json") ? "public, max-age=300" : "public, max-age=86400");

    // onlyIf failed: the client's copy is current.
    if (request.method === "GET" && !("body" in object))
      return new Response(null, { status: 304, headers });

    if (request.method === "HEAD") {
      headers.set("Content-Length", String(object.size));
      return new Response(null, { status: 200, headers });
    }

    if (object.range && request.headers.has("Range")) {
      const offset = object.range.offset ?? object.size - object.range.suffix;
      const length = object.range.length ?? object.size - offset;
      headers.set("Content-Range", `bytes ${offset}-${offset + length - 1}/${object.size}`);
      headers.set("Content-Length", String(length));
      return new Response(object.body, { status: 206, headers });
    }

    headers.set("Content-Length", String(object.size));
    return new Response(object.body, { status: 200, headers });
  },
};
