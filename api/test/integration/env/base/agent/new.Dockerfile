FROM public.ecr.aws/o5x5t0j3/amd64/api_development:integration_test_wazuh-generic

ARG WAZUH_BRANCH

## install Wazuh
RUN mkdir wazuh && curl -sL https://github.com/wazuh/wazuh/tarball/${WAZUH_BRANCH} | tar zx --strip-components=1 -C wazuh
ADD base/agent/preloaded-vars.conf /wazuh/etc/preloaded-vars.conf
RUN /wazuh/install.sh

COPY base/agent/entrypoint.sh /scripts/entrypoint.sh

HEALTHCHECK --retries=300 --interval=1s --timeout=30s --start-period=30s CMD /usr/bin/python3 /tmp/healthcheck/healthcheck.py || exit 1
