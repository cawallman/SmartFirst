import socket, os
from gtts import gTTS
from pydub import AudioSegment
import speech_recognition as sr

HOST, PORT = '0.0.0.0', 1234
PLAY_TRIGGER = False
SPEECH_PATH = 'AudioOutput/outputSpeech.wav'
REC_PATH = 'AudioOutput/received.wav'

def process_audio():
    global PLAY_TRIGGER
    recognizer = sr.Recognizer()
    
    try:
        # REPAIR STEP: Load with pydub first to fix headers
        audio_fix = AudioSegment.from_file(REC_PATH)
        audio_fix.export(REC_PATH, format="wav") # Rewrites a perfect header
        
        with sr.AudioFile(REC_PATH) as source:
            audio = recognizer.record(source)
            text = recognizer.recognize_google(audio)
            print(f"User said: {text}")
            
            # Generate response
            tts = gTTS(text=f"You said {text}", lang='en')
            tts.save(SPEECH_PATH)
            
            # Resample for ESP32 Playback
            resampled = AudioSegment.from_file(SPEECH_PATH)
            resampled.set_frame_rate(22050).set_channels(1).export(SPEECH_PATH, format="wav")
            
            PLAY_TRIGGER = True
    except Exception as e:
        print(f"STT Error Detail: {e}")

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((HOST, PORT))
s.listen(5)

print(f"Server listening on {PORT}...")

while True:
    conn, addr = s.accept()
    try:
        # 1. Read the initial chunk (contains headers)
        request_data = conn.recv(4096)
        if not request_data: continue
        
        header_text = request_data.decode('latin-1')
        
        if "POST /upload" in header_text:
            # Look for Content-Length
            content_length = 0
            for line in header_text.split('\r\n'):
                if "Content-Length:" in line:
                    content_length = int(line.split(':')[1].strip())
                    
            print(f"ESP32 says it is sending: {content_length} bytes")
            
            if content_length <= 44: # 44 bytes is just a header with no audio
                print("Ignored: ESP32 sent an empty or header-only file.")
                conn.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
                conn.close()
                continue

            # 2. Find exactly where the header ends
            header_end_marker = b'\r\n\r\n'
            header_end_pos = request_data.find(header_end_marker)
            
            # The 'body' starts immediately after \r\n\r\n
            body = request_data[header_end_pos + 4:]
            
            # 3. Keep receiving until we hit the Content-Length
            while len(body) < content_length:
                chunk = conn.recv(4096)
                if not chunk: break
                body += chunk
            
            print(f"Actually received: {len(body)} bytes")

            with open(REC_PATH, 'wb') as f:
                f.write(body)
            
            conn.sendall(b"HTTP/1.1 200 OK\r\n\r\n")
            
            with open(REC_PATH, 'wb') as f:
                f.write(body)
            
            print(f"File saved ({len(body)} bytes). Starting STT...")
            process_audio()

        elif "GET /check_status" in data:
            resp = "PLAY" if PLAY_TRIGGER else "WAIT"
            msg = f"HTTP/1.1 200 OK\r\nContent-Length: {len(resp)}\r\nConnection: close\r\n\r\n{resp}"
            conn.sendall(msg.encode())
            if PLAY_TRIGGER: PLAY_TRIGGER = False

        elif "GET /download" in data:
            if os.path.exists(SPEECH_PATH):
                with open(SPEECH_PATH, 'rb') as f:
                    content = f.read()
                header = f"HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: {len(content)}\r\nConnection: close\r\n\r\n"
                conn.sendall(header.encode() + content)
    except Exception as e:
        print(f"Waiting...")
    finally:
        conn.close()