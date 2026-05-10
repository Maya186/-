#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Remote File Storage Server with REST API over HTTP
Поддерживаемые методы: GET, PUT, HEAD, DELETE
"""

import os
import shutil
import mimetypes
from datetime import datetime, timezone
from flask import Flask, request, jsonify, Response, send_file

app = Flask(__name__)

STORAGE_ROOT = os.path.abspath(os.getenv('STORAGE_ROOT', './storage'))
AUTH_TOKEN = os.getenv('AUTH_TOKEN', 'secret-token-123')
MAX_CONTENT_LENGTH = 100 * 1024 * 1024  # 100 MB

os.makedirs(STORAGE_ROOT, exist_ok=True)
app.config['MAX_CONTENT_LENGTH'] = MAX_CONTENT_LENGTH


def normalize_path(requested_path: str) -> str:
    """Безопасное преобразование URL-пути в путь на диске"""
    clean = os.path.normpath(requested_path.strip('/'))
    target = os.path.abspath(os.path.join(STORAGE_ROOT, clean))
    # Защита от path traversal
    if not (target == STORAGE_ROOT or target.startswith(STORAGE_ROOT + os.sep)):
        return None
    return target


def format_http_date(timestamp: float) -> str:
    """Формат даты для HTTP-заголовков (RFC 7231)"""
    return datetime.fromtimestamp(timestamp, tz=timezone.utc).strftime(
        '%a, %d %b %Y %H:%M:%S GMT'
    )


@app.route('/<path:filepath>', methods=['GET', 'HEAD', 'PUT', 'DELETE'])
@app.route('/', methods=['GET', 'HEAD', 'PUT', 'DELETE'])
def handle_request(filepath=''):
    """Единый обработчик всех запросов"""
    if filepath:
        target_path = normalize_path(filepath)
    else:
        target_path = STORAGE_ROOT
    
    if target_path is None:
        return jsonify({"error": "Forbidden", "message": "Invalid path"}), 403

    # ==================== GET ====================
    if request.method == 'GET':
        if os.path.isfile(target_path):
            mime_type, _ = mimetypes.guess_type(target_path)
            return send_file(target_path, mimetype=mime_type or 'application/octet-stream')
        elif os.path.isdir(target_path):
            items = []
            for entry in sorted(os.scandir(target_path), key=lambda e: e.name):
                try:
                    stat = entry.stat()
                    items.append({
                        "name": entry.name,
                        "type": "directory" if entry.is_dir() else "file",
                        "size": stat.st_size if entry.is_file() else None,
                        "modified": datetime.fromtimestamp(stat.st_mtime).isoformat(),
                        "url": f"{request.url.rstrip('/')}/{entry.name}"
                    })
                except (PermissionError, OSError):
                    continue
            return jsonify({
                "path": request.path,
                "url": request.url,
                "items": items,
                "total": len(items)
            }), 200
        else:
            return jsonify({"error": "Not found", "path": filepath}), 404

    # ==================== HEAD ====================
    elif request.method == 'HEAD':
        if os.path.isfile(target_path):
            stat = os.stat(target_path)
            mime_type, _ = mimetypes.guess_type(target_path)
            resp = Response(status=200)
            resp.headers['Content-Length'] = stat.st_size
            resp.headers['Last-Modified'] = format_http_date(stat.st_mtime)
            resp.headers['Content-Type'] = mime_type or 'application/octet-stream'
            resp.headers['Accept-Ranges'] = 'bytes'
            return resp
        elif os.path.isdir(target_path):
            resp = Response(status=200)
            resp.headers['Content-Type'] = 'application/json'
            return resp
        else:
            return jsonify({"error": "Not found", "path": filepath}), 404

        # ========== PUT ==========
    elif request.method == 'PUT':
        token = request.headers.get('Authorization', '').replace('Bearer ', '').strip()
        if not token or token != AUTH_TOKEN:
            return jsonify({"error": "Unauthorized", "message": "Valid token required"}), 401

        if os.path.isdir(target_path):
            return jsonify({"error": "Bad request", "message": "Cannot PUT to directory"}), 400

        copy_from = request.headers.get('X-Copy-From')
        if copy_from:
            source_path = normalize_path(copy_from)
            if not source_path or not os.path.isfile(source_path):
                return jsonify({"error": "Not found", "message": "Source file not found"}), 404
            parent = os.path.dirname(target_path)
            if parent:
                os.makedirs(parent, exist_ok=True)
            shutil.copy2(source_path, target_path)
            return jsonify({
                "message": "File copied successfully",
                "source": copy_from,
                "destination": filepath,
                "size": os.path.getsize(target_path)
            }), 201

        raw_body = request.get_data()
        
        file_exists = os.path.isfile(target_path)

        parent = os.path.dirname(target_path)
        if parent:
            os.makedirs(parent, exist_ok=True)

        try:
            if request.files and 'file' in request.files and request.files['file'].filename:
                request.files['file'].save(target_path)
            elif raw_body:
                # Сырые данные
                with open(target_path, 'wb') as f:
                    f.write(raw_body)
            elif request.form:
                content = next(iter(request.form.values()), '')
                with open(target_path, 'w', encoding='utf-8') as f:
                    f.write(content if isinstance(content, str) else str(content))
            else:
                return jsonify({"error": "Bad request", "message": "No content provided"}), 400

            if file_exists:
                # Файл был обновлён
                return jsonify({
                    "message": "File updated successfully",
                    "path": filepath,
                    "size": os.path.getsize(target_path)
                }), 200  # или 204 No Content
            else:
                # Файл создан впервые
                return jsonify({
                    "message": "File created successfully",
                    "path": filepath,
                    "size": os.path.getsize(target_path)
                }), 201

        except PermissionError:
            return jsonify({"error": "Forbidden", "message": "Write denied"}), 403
        except OSError as e:
            return jsonify({"error": "Internal error", "message": str(e)}), 500    # ==================== DELETE ====================
    elif request.method == 'DELETE':
        token = request.headers.get('Authorization', '').replace('Bearer ', '').strip()
        if not token or token != AUTH_TOKEN:
            return jsonify({"error": "Unauthorized", "message": "Valid token required"}), 401

        if not os.path.exists(target_path):
            return jsonify({"error": "Not found", "path": filepath}), 404

        try:
            if os.path.isfile(target_path):
                os.remove(target_path)
                return jsonify({"message": "File deleted", "path": filepath}), 204
            elif os.path.isdir(target_path):
                if target_path == STORAGE_ROOT:
                    return jsonify({"error": "Forbidden", "message": "Cannot delete root"}), 403
                shutil.rmtree(target_path)
                return jsonify({"message": "Directory deleted", "path": filepath}), 204
        except PermissionError:
            return jsonify({"error": "Forbidden", "message": "Delete denied"}), 403
        except OSError as e:
            return jsonify({"error": "Internal error", "message": str(e)}), 500

    # ==================== METHOD NOT ALLOWED ====================
    else:
        return jsonify({"error": "Method not allowed", "allowed": ["GET", "HEAD", "PUT", "DELETE"]}), 405


# ==================== ОБРАБОТКА ОШИБОК ====================

@app.errorhandler(404)
def not_found(e):
    return jsonify({"error": "Not found", "message": "Resource does not exist"}), 404

@app.errorhandler(405)
def method_not_allowed(e):
    return jsonify({"error": "Method not allowed"}), 405

@app.errorhandler(403)
def forbidden(e):
    return jsonify({"error": "Forbidden", "message": str(e.description)}), 403

@app.errorhandler(400)
def bad_request(e):
    return jsonify({"error": "Bad request", "message": str(e.description)}), 400

@app.errorhandler(413)
def too_large(e):
    return jsonify({"error": "Payload too large", "message": f"Max: {MAX_CONTENT_LENGTH // (1024*1024)} MB"}), 413

@app.errorhandler(500)
def internal_error(e):
    return jsonify({"error": "Internal server error"}), 500


# ==================== HEALTH CHECK ====================

@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        "status": "ok",
        "storage_root": STORAGE_ROOT,
        "timestamp": datetime.now(timezone.utc).isoformat()
    }), 200


# ==================== ЗАПУСК ====================

if __name__ == '__main__':
    print("=" * 60)
    print("️  Remote File Storage Server")
    print("=" * 60)
    print(f" Root: {STORAGE_ROOT}")
    print(f" Token: {AUTH_TOKEN}")
    print(f" URL: http://localhost:5000")
    print("=" * 60)
    print("Методы: GET, HEAD, PUT, DELETE")
    print("=" * 60)
    app.run(host='0.0.0.0', port=5000, debug=False)
