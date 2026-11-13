require 'interceptor'
Interceptor::Interceptor.open { |client| p client.info }
