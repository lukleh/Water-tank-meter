from flask import Flask, send_file, send_from_directory, jsonify, request

app = Flask(__name__, static_folder='data')

@app.route("/")
def index():
    """
    static serve index.html
    """
    return send_file("data/index.html")

@app.route("/data")
def data():
    """
    return json data as real sensor does
    """
    return send_file("test_data/sensordata.json")

@app.route("/save", methods=['POST'])
def save():
    """
    check that post form is sending correct data, return it back
    """
    data = request.form
    return jsonify(data)

@app.route('/<path:name>')
def js_sensor(name):
    """
    static serve main js file
    """
    return send_from_directory(app.static_folder, name)
