FROM python:3-slim-bullseye
RUN apt-get update && apt-get upgrade -y
RUN apt-get install -y ffmpeg python3-soundfile
WORKDIR /usr/lib/rvtool
COPY requirements.txt requirements.txt
RUN pip3 install -r requirements.txt
COPY rvtool.py /usr/bin/rvtool
RUN chmod 755 /usr/bin/rvtool
ENTRYPOINT [ "python3", "/usr/bin/rvtool" ]
CMD [ "-h" ]
