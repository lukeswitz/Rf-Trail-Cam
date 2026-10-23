FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app.py .
COPY templates/ templates/

RUN mkdir -p instance

ENV FLASK_DEBUG=0

EXPOSE 5000

CMD ["python", "app.py"]
