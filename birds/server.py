from birdnetlib import Recording
from birdnetlib.analyzer import Analyzer
from flask import Flask, request, jsonify
import psycopg2
from psycopg2.extras import RealDictCursor
import os
import json
import logging
import datetime

# configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# load and initialize the BirdNET-Analyzer models
analyzer = Analyzer()

# server
app = Flask(__name__)
app.secret_key = 'your-secret-key-here'
UPLOAD_FOLDER = "new_received_audio"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# database connection
def get_db_connection():
    conn = psycopg2.connect(
        host="localhost",
        port=5400,
        dbname="myapp",
        user="postgres",
        password="password123"
    )
    return conn

def add_bird(prediction, audio_name, time, lon, lat):
    conn = get_db_connection()
    cur = conn.cursor(cursor_factory=psycopg2.extras.DictCursor)
    
    query = """
    INSERT INTO birds 
    (common_name, scientific_name, start_time, end_time, detection_date, confidence, label, audio_file, lon, lat)
    VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
    RETURNING id;
    """
    values = (
        prediction['common_name'],
        prediction['scientific_name'], 
        prediction['start_time'],
        prediction['end_time'],
        time,
        prediction['confidence'],
        prediction['label'],
        audio_name,
        lon,
        lat
    )
    
    try:
        cur.execute(query, values)
        new_id = cur.fetchone()['id']
        conn.commit()
        print(f"Inserted detection with ID: {new_id}")
        return new_id
    except Exception as e:
        conn.rollback()
        print(f"Error inserting detection: {e}")
        return None
    finally:
        cur.close()
        conn.close()

def analyze_audio(filename, date):
    recording = Recording(
        analyzer,
        filename,
        lon=24,
        lat=56,
        week_48=21
    )
    
    recording.analyze()
    
    for i in recording.detections:
        print(i)
        add_bird(i, filename, date, 24, 56)

@app.route("/upload", methods=["POST"])
def upload_file():
    metadata = json.loads(request.form.get('metadata'))
    file = request.files.get('audio')
    print(metadata, file.filename)

    # create safe filename
    timestamp_str = metadata.get('time_string', datetime.datetime.now().isoformat())
    safe_filename = f"recording_{timestamp_str.replace(':', '-').replace(' ', '_')}.wav"
    audio_path = os.path.join(UPLOAD_FOLDER, safe_filename)
    print("AUDIO PATH",audio_path)

    # save file
    file.save(audio_path)
    logger.info(f"Saved audio file to {audio_path}")

    # analyze the file
    analyze_audio(audio_path, timestamp_str)

    return jsonify({
        'status': 'success',
        'filename': safe_filename,
        'timestamp': timestamp_str
    }), 200
    
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8000)
