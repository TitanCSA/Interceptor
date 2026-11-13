# Interceptor Ruby SDK

`gem install interceptor-sdk`

Requires Ruby 3.1+ on Windows x64. Direct Fiddle bindings to this product's native SDK, with no Python dependency. Supported families: keyboard, mouse, hid, xinput, xinputhid, gip. Driver and original licensing requirements remain unchanged.

```ruby
require 'interceptor_sdk'
InterceptorSDK::Interceptor.open do |client|
  p client.info
end
```

The block form closes the native session even on an exception. Importing/constructing the SDK does not install a driver or start capture. See [API documentation](docs/API.md).

## Version 2 namespace

Use `require 'interceptor_sdk'` and `InterceptorSDK::Interceptor`. Version 1 used the generic `interceptor` require/module name, which can conflict with unrelated gems. Version 1 artifacts remain unchanged; version 2 deliberately uses an isolated namespace.
