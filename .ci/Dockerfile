FROM ubuntu:24.04
COPY .ci /opt/gki/.ci
RUN bash /opt/gki/.ci/install-deps.sh
ENV PATH="/opt/gki/out/ci-venv/bin:${PATH}"
WORKDIR /work
