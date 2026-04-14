import socket, os, time
from gtts import gTTS
from pydub import AudioSegment
import speech_recognition as sr
import requests
import json
import uuid

HOST, PORT = '0.0.0.0', 1234
PLAY_TRIGGER = False
SPEECH_PATH = 'AudioOutput/outputSpeech.wav'
REC_PATH = 'AudioOutput/received.wav'

API_URL = "https://1xlhqdcbnj.execute-api.us-east-2.amazonaws.com/prod/ask"

CURRENT_SESSION_ID = str(uuid.uuid4())

if not os.path.exists('AudioOutput'):
    os.makedirs('AudioOutput')
    
def ask_smartfirst(question: str, session_id: str): 
    payload = {
        "session_id": session_id, # Use the passed session_id instead of a global
        "query": question
    }
    
    try:
        response = requests.post(
            API_URL,
            headers={"Content-Type": "application/json"},
            data=json.dumps(payload),
            timeout=120
        )
        
        print("Status:", response.status_code)
        
        data = response.json()
        
        print("\n----- SOURCES -----\n")
        for s in data.get("sources", []):
            print(s)
        
        
        return data.get("response", "No response")
    except Exception as e:
        print("Request failed:", e)

def process_audio(header_text):
    global CURRENT_SESSION_ID, PLAY_TRIGGER
    
    if "X-Reset-Session: true" in header_text:
        CURRENT_SESSION_ID = str(uuid.uuid4())
        print(f"--- NEW SESSION STARTED: {CURRENT_SESSION_ID} ---")

    recognizer = sr.Recognizer()
    try:
        # Check if file exists and has data
        if not os.path.exists(REC_PATH) or os.path.getsize(REC_PATH) < 44:
            print("Error: Received file is too small or missing.")
            return

        # Attempt to fix/load the WAV
        try:
            audio_fix = AudioSegment.from_file(REC_PATH)
            audio_fix.export(REC_PATH, format="wav") 
        except Exception as e:
            print(f"FFmpeg/Pydub could not read the file: {e}")
            return
        
        with sr.AudioFile(REC_PATH) as source:
            audio_data = recognizer.record(source)
            text = recognizer.recognize_google(audio_data)
            print(f"User said: {text}")
            
            api_response = ask_smartfirst(text, CURRENT_SESSION_ID)
            
            # gTTS and conversion logic...
            tts = gTTS(api_response, lang='en')
            temp_mp3 = "AudioOutput/temp.mp3"
            tts.save(temp_mp3)
            audio = AudioSegment.from_file(temp_mp3)
            audio = audio.speedup(playback_speed=1.3) 
            audio = audio.set_frame_rate(22050).set_channels(1).set_sample_width(2)
            audio.export(SPEECH_PATH, format="wav")
            
            PLAY_TRIGGER = True
            print("Ready to stream.")

    except Exception as e:
        print(f"Speech Recognition Error: {e}")

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((HOST, PORT))
s.listen(5)
s.settimeout(240)
print(f"Server listening on {PORT}...")

while True:
    conn, addr = s.accept()
    try:
        conn.settimeout(20.0) 
        
        # --- NEW ACCUMULATION LOGIC ---
        request_data = b""
        header_end_marker = b'\r\n\r\n'
        
        # Keep reading until we see the end of the headers
        while header_end_marker not in request_data:
            chunk = conn.recv(1024)
            if not chunk: break
            request_data += chunk
            if len(request_data) > 8192: break # Safety limit

        if not request_data or header_end_marker not in request_data:
            print("Error: Could not find end of HTTP headers in time.")
            conn.close()
            continue
        
        header_text = request_data.decode('latin-1')
        
        if "POST /upload" in header_text:
            content_length = 0
            for line in header_text.split('\r\n'):
                if "Content-Length:" in line:
                    content_length = int(line.split(':')[1].strip())
            
            print(f"Expecting: {content_length} bytes")
            
            # 1. FIND THE EXACT SPLIT POINT
            header_end_pos = request_data.find(header_end_marker)
            
            # 2. CAPTURE THE REMAINDER (The start of the audio)
            # This is the crucial step: grabbing data already read into 'request_data'
            body = request_data[header_end_pos + 4:] 
            
            # 3. READ THE REMAINING BYTES ONLY
            # We subtract what we already have in 'body' from the 'content_length'
            while len(body) < content_length:
                to_read = min(4096, content_length - len(body))
                chunk = conn.recv(to_read)
                if not chunk: 
                    break
                body += chunk
            
            print(f"Successfully received: {len(body)} bytes")

            if len(body) > 0:
                with open(REC_PATH, 'wb') as f:
                    f.write(body)
                
                process_audio(header_text)
                
                # Send the response
                response = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 16\r\nConnection: close\r\n\r\nTRIGGER_DOWNLOAD"
                conn.sendall(response)
                
                # Add a tiny sleep to ensure the OS buffer pushes the data out before closing the socket
                time.sleep(0.5) 
                print("Trigger sent to ESP32")
            else:
                print("Received 0 bytes of audio data.")

        elif "GET /download" in header_text:
            if os.path.exists(SPEECH_PATH):
                with open(SPEECH_PATH, 'rb') as f:
                    content = f.read()
                header = f"HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: {len(content)}\r\nConnection: close\r\n\r\n"
                conn.sendall(header.encode() + content)

    except Exception as e:
        print(f"Socket Error: {e}")
    finally:
        conn.close()


