FROM n0madic/alpine-gcc:9.2.0

RUN apk add --no-cache --upgrade \
  curl>=7.66 \
  curl-dev>=7.66 \
  linux-headers

COPY . /app
WORKDIR /app

RUN sed -i -e 's/\r$//' /app/run.sh

ENTRYPOINT ["/app/run.sh"]