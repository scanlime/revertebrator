FROM python:3-slim-bullseye
RUN apt-get update && apt-get upgrade -y
RUN apt-get install -y python3-soundfile
WORKDIR /work
COPY requirements.txt requirements.txt
RUN pip3 install -r requirements.txt
COPY . .
ENTRYPOINT [ "python3", "/work/rvtool.py" ]
CMD [ "-h" ]
