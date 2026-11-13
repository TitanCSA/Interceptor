const {Interceptor}=require("interceptor-sdk");const io=new Interceptor();try{console.log(io.info())}finally{io.close()}
