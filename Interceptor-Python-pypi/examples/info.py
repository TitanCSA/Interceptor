from interceptor import Client
with Client() as client:
    print(client.info())
