<?php
/* vim: set syn=php: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

require_once("parapara.inc");
require_once("db.inc");

class Design {
  public $id       = null;
  public $name     = null;
  public $duration = null;
  // virtual: thumbnail
  // virtual: video
  // virtual: editorStylesheet
  // virtual: wall
  private $virtualFields = array();
  private $filePath = null;

  public function __construct($id, $name, $duration) {
    $this->id       = intval($id);
    $this->name     = $name;
    $this->duration = intval($duration);
    $this->filePath = dirname(__FILE__) . '/../public/designs/' . $name . '/';
  }

  public function __get($name) {
    if (array_key_exists($name, $this->virtualFields)) {
      return $this->virtualFields[$name];
    }

    $result = null;
    switch ($name) {
      case 'thumbnail':
        $result = $this->getThumbnail();
        break;
      case 'video':
        $result = $this->getVideos();
        break;
      case 'editorStylesheet':
        $result = $this->getEditorStylesheet();
        break;
      case 'wall':
        $result = $this->getWall();
        break;
      default:
        error_log('Unknown property: ' . $name);
        return null;
    }

    $this->virtualFields[$name] = $result;
    return $result;
  }

  protected function getThumbnail() {
    $extensions = array('png', 'jpg', 'jpeg', 'gif');
    foreach ($extensions as $extension) {
      if (file_exists($this->filePath . 'preview/' . $this->name .
                      '.' . $extension)) {
        return '/designs/' . $this->name . '/preview/' . $this->name
               . '.' . $extension;
      }
    }
    return null;
  }

  protected function getVideos() {
    $extensions = array('mp4', 'webm');
    $videos = array();
    foreach ($extensions as $extension) {
      if (file_exists($this->filePath . 'preview/' . $this->name .
                      '.' . $extension)) {
        $videos[] = '/designs/' . $this->name . '/preview/' . $this->name
                  . '.' . $extension;
      }
    }
    return count($videos) ? $videos : null;
  }

  protected function getEditorStylesheet() {
    return file_exists($this->filePath . 'editor/editor.css')
        ? '/designs/' . $this->name . '/editor/editor.css'
        : null;
  }

  protected function getWall() {
    $file = file_exists($this->filePath . 'wall.html')
          ? 'wall.html'
          : (file_exists($this->filePath . 'wall.svg') ? 'wall.svg' : null);
    return $file ? '/designs/' . $this->name . '/' . $file : null;
  }

  // Return a form suitable for serializing over JSON
  public function asArray() {
    return array(
      'designId'         => $this->id,
      'name'             => $this->name,
      'duration'         => $this->duration,
      'thumbnail'        => $this->thumbnail,
      'video'            => $this->video,
      'editorStylesheet' => $this->editorStylesheet,
      'wall'             => $this->wall
    );
  }
}

class DesignGallery {
  private static $designs = array();
  private static $allLoaded = false;

  public static function getById($id) {
    // Sanitize input
    $id = intval($id);

    // Check if we already have the result cached
    if (array_key_exists($id, self::$designs))
      return self::$designs[$id];

    // If we've loaded all designs and it's still not found then it mustn't 
    // exist
    if (self::$allLoaded)
      return null;

    // Otherwise load it
    $conn =& getDbConnection();
    $row =& $conn->queryRow(
      'SELECT designId, name, duration FROM designs WHERE designId = '
      . $conn->quote($id, 'integer')
      . ' LIMIT 1',
      null,
      MDB2_FETCHMODE_ASSOC
    );
    checkDbResult($row);

    // Check if it was found
    if ($row === null)
      return null;

    // Otherwise store and return
    self::$designs[$id] = new Design($id, $row['name'], $row['duration']);
    return self::$designs[$id];
  }

  public static function getByWallPath($path) {
    // Look up path in database
    $conn =& getDbConnection();
    $row =& $conn->queryRow(
        'SELECT designs.designId, designs.name, designs.duration FROM designs'
      . ' INNER JOIN walls ON designs.designId = walls.designId'
      . ' WHERE walls.urlPath = ' . $conn->quote($path, 'text')
      . ' LIMIT 1',
      null,
      MDB2_FETCHMODE_ASSOC);
    checkDbResult($row);

    // Check if it was found
    if ($row === null)
      return null;

    // Check if we already have it cached
    $id = intval($row['designid']);
    if (array_key_exists($id, self::$designs))
      return self::$designs[$id];

    // Otherwise store and return
    self::$designs[$id] = new Design($id, $row['name'], $row['duration']);
    return self::$designs[$id];
  }

  public static function getAll() {
    // Return cached result if we already have it
    if (self::$allLoaded)
      return self::$designs;

    // Query all designs
    $conn =& getDbConnection();
    $res =& $conn->query(
      'SELECT designId, name, duration FROM designs ORDER BY designId');
    checkDbResult($res);

    // Create Design objects
    $result = array();
    $conn->setFetchMode(MDB2_FETCHMODE_ASSOC);
    while ($row = $res->fetchRow()) {
      $result[intval($row['designid'])] =
        new Design($row['designid'], $row['name'], $row['duration']);
    }

    // Cache result
    self::$designs = $result;
    self::$allLoaded = true;

    return array_values(self::$designs);
  }

  public static function validateDesignId($id) {
    $design = self::getById($id);
    if ($design === null)
      throw new KeyedException('bad-design');
  }
}

// Get the summary of all designs as an array suitable for JSON encoding
//
// The fields are as follows:
//
//   name - design name (not needed?)
//   (thumbnail) - the thumbnail graphic, if any, used to represent this design
//   (video) - the video preview, if any, used to represent this design
//   duration - the default duration for this design
//
function getDesignSummary() {
  $result = array();
  $designs = DesignGallery::getAll();
  foreach ($designs as $design) {
    // Convert to array
    $designArray = $design->asArray();

    // Filter out empty values
    if ($designArray['thumbnail'] === null)
      unset($designArray['thumbnail']);
    if ($designArray['video'] === null || count($designArray['video']) === 0)
      unset($designArray['video']);

    $result[] = $designArray;
  }
  return $result;
}

?>
