FROM 192.168.1.204:5050/rv1106g3/zh_uclibc_linux_client/zh-uclibc-linux-client-runner:20260309

RUN set -eux; \
    if command -v apt-get >/dev/null 2>&1; then \
      apt-get update; \
      apt-get install -y --no-install-recommends make; \
      rm -rf /var/lib/apt/lists/*; \
    elif command -v apk >/dev/null 2>&1; then \
      apk add --no-cache make; \
    elif command -v dnf >/dev/null 2>&1; then \
      dnf install -y make && dnf clean all; \
    elif command -v yum >/dev/null 2>&1; then \
      yum install -y make && yum clean all; \
    elif command -v opkg >/dev/null 2>&1; then \
      opkg update && opkg install make; \
    else \
      echo "No supported package manager found"; \
      exit 1; \
    fi

RUN make --version | head -n1
