# The AI sidecar.
#
# Separate from the engine image because it has a completely different
# dependency set and a completely different reason to be restarted: prompts and
# model choices change often, the C++ engine does not.
#
# The OpenAI API key is never baked in. It arrives as an environment variable
# at run time (docker-compose reads it from the host or from .env), and the
# service starts and reports "not ready" without one rather than failing.

FROM python:3.11-slim AS runtime

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

COPY python/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir --requirement requirements.txt

COPY python/ ./python/

RUN useradd --create-home --shell /usr/sbin/nologin sidecar \
    && chown -R sidecar:sidecar /app
USER sidecar

EXPOSE 8810

HEALTHCHECK --interval=15s --timeout=3s --start-period=5s --retries=3 \
    CMD python -c "import urllib.request,sys; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:8810/health', timeout=2).status == 200 else 1)"

CMD ["uvicorn", "python.ai.service:app", "--host", "0.0.0.0", "--port", "8810"]
