# Claw web integration

Optional Claw sources inside the `http_server` component. Enable
`CONFIG_HTTP_SERVER_CLAW_ENABLE` (default `y`) and attach the extension
returned by `http_server_claw_init()` before starting the base server.

This directory contains capability and Lua module lists, WeChat login, WebIM,
and optional Lua HTTP routes. File management stays in the base server.
Supply WeChat callbacks to register login routes; local chat and Lua routes
also require their existing capability switches.

Configuration storage belongs to the application. Both examples use the
shared configuration adapter under `examples/common`; the HTTP extension
does not own or reset the stored configuration. These sources and their Claw
dependencies are included only when the web feature is enabled.
