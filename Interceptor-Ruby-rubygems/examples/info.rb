require 'interceptor_sdk'
InterceptorSDK::Interceptor.open { |client| p client.info }
