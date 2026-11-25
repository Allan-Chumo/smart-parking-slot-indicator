FROM nodered/node-red:4.0.9
RUN npm install --no-audit --no-fund \
    @flowfuse/node-red-dashboard@1.37.0 \
    node-red-node-sqlite@2.0.1
USER root
RUN apk add --no-cache sqlite && mkdir /db && chown node-red /db
USER node-red
