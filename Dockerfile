FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app.py .
COPY templates/ templates/

RUN useradd --create-home --uid 10001 app \
    && mkdir -p instance \
    && chown app:app instance

USER app

ENV SESSION_COOKIE_SECURE=1
ENV TRUST_CF_CONNECTING_IP=1

EXPOSE 5000

CMD ["sh", "-c", "python -c 'from app import app, db; app.app_context().push(); db.create_all()' && exec gunicorn --bind 0.0.0.0:5000 --workers 1 --threads 4 app:app"]
