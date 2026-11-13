from interceptor_sdk import Client
with Client() as client:
    print(client.info())
