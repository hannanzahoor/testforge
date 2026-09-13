# The sample service — the system under test.
#
# Small on purpose: it exists so TestForge has something realistic to point at,
# and so the whole stack can be brought up with one docker-compose command.

FROM python:3.11-slim AS runtime

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

# Requirements first, so a change to the application code does not reinstall
# the dependency layer.
COPY requirements.txt ./
RUN pip install --no-cache-dir --requirement requirements.txt

COPY app/ ./app/

RUN useradd --create-home --shell /usr/sbin/nologin service \
    && chown -R service:service /app
USER service

EXPOSE 8000

# curl is not installed, so the health check uses the interpreter that is
# already here rather than adding a package for one command.
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=5 \
    CMD python -c "import urllib.request,sys; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:8000/health', timeout=2).status == 200 else 1)"

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
