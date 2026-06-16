FROM node:20-alpine
WORKDIR /app
COPY package*.json ./
RUN npm ci --omit=dev
COPY data/ ./data/
COPY server/ ./server/
USER node
EXPOSE 8080
CMD ["node", "server/index.js"]
