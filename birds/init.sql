CREATE TABLE IF NOT EXISTS birds (
    id SERIAL PRIMARY KEY,
    common_name VARCHAR(100) NOT NULL,
    scientific_name VARCHAR(100) NOT NULL,
    start_time DECIMAL(10,3) NOT NULL,
    end_time DECIMAL(10,3) NOT NULL,
    confidence DECIMAL(15,12) NOT NULL,
    label VARCHAR(200) NOT NULL,
    detection_date TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    audio_file VARCHAR(255),
    lon DECIMAL(10,3),
    lat DECIMAL(10,3)
)
