<?php
/* vim: set syn=php: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

require_once("parapara.inc");
require_once("exceptions.inc");
require_once("UriUtils.inc");
require_once("walls.inc");
require_once("characters.email.inc");
require_once("characters.makeapi.inc");

class Character {
  private $_charId          = null;
  private $_wall            = null;
  private $fields           = null;
  private $dirtyFields      = array();

  static private $virtualFields =
    array('rawUrl','galleryUrl', 'emailUrl', 'previewUrl', 'wall');

  static private $readOnlyFields = array('charId');

  public function __construct($charId, $fields) {
    $this->_charId  = $charId;
    $this->fields   = $fields;
  }

  public function __get($name) {
    // Check if the field is set
    if (!$this->__isset($name)) {
      return null;
    }

    // Regular fields
    if (array_key_exists($name, $this->fields)) {
      return $this->fields[$name];
    }

    // Virtual fields
    switch ($name) {
      case 'charId':
        return $this->_charId;

      case 'rawUrl':
        return Character::getRawUrl($this->_charId);

      case 'galleryUrl':
        return Character::getGalleryUrl($this->_charId);

      case 'previewUrl':
        return Character::getPreviewUrl($this->_charId);

      case 'emailUrl':
        return Character::getEmailUrl($this->_charId);

      case 'wall':
        // We call isset above which will fill this in
        return $this->_wall;
    }

    return null;
  }

  public function __isset($name) {
    // Regular fields
    if (array_key_exists($name, $this->fields))
      return true;

    // Virtual fields
    switch ($name) {
      case 'charId':
      case 'rawUrl':
      case 'galleryUrl':
      case 'previewUrl':
        return true;

      case 'emailUrl':
        return CharacterEmailer::isEmailEnabled();

      case 'wall':
        if ($this->_wall === null) {
          $this->_wall = Walls::getById($this->wallId);
        }
        return $this->_wall !== null;
    }

    // Semi-virtual fields
    if ($name == 'galleryUrlShort') {
      return $this->attemptToFillInShortUrl();
    }

    return false;
  }

  // This method is called when we don't have a saved shortened url for the 
  // character.
  //
  // Typically we save a shortened URL when creating a character but that might 
  // not always be the case for older characters, or if the URL shortening 
  // service was unavailable, or if we change the URL scheme and blow away all 
  // the shortened-URL fields.
  //
  // In those cases we attempt to create a short URL. If we succeed, we don't 
  // save it immediately but just mark the field as dirty.
  //
  // If we really need to do a mass update of fields, there is a script for that
  // (see ../tools/fillInShortUrls.php).
  private function attemptToFillInShortUrl() {
    if (isset($this->fields['galleryUrlShort'])) {
      error_log("Calling attemptToFillInShortUrl when there already is one!");
      return true;
    }

    // Try to get a short URL
    $shortUrl = Character::getShortGalleryUrl($this->_charId);
    if (!$shortUrl)
      return false;

    // Update the fields and mark this field as needing to be saved
    $this->fields['galleryUrlShort'] = $shortUrl;
    array_push($this->dirtyFields, 'galleryUrlShort');

    return true;
  }

  public function __set($name, $value) {
    // Check authorisation
    if (!$this->canAdminister())
      throw new KeyedException('no-auth');

    // Check if we recognize the field
    if (!$this->__isset($name))
      throw new KeyedException('unknown-field');

    // Sanitize values
    switch($name) {
      case 'active':
        $value = (bool)$value;
        break;

      case 'x':
        $value = self::sanitizeX($value);
        break;

      // If we ever support setting the title and author, we'll need to rewrite 
      // the metadata in the SVG file too

      default:
        throw new KeyedException('readonly-field');
    }

    // Check if the value is actually different
    if ($this->$name === $value)
      return;

    // Regular fields
    if (array_key_exists($name, $this->fields)) {
      $this->fields[$name] = $value;
      // Mark field as dirty
      if (!in_array($name, $this->dirtyFields))
        array_push($this->dirtyFields, $name);
    }
  }

  public function save() {
    // Don't check for authorization here. That is done in __set.
    // This allows non-logged in users to query galleryUrlShort and then save 
    // which will cause the short URL to be cached.

    // Check there is something to save
    if (count($this->dirtyFields) === 0)
      return array();

    // Start connection
    $conn =& getDbConnection();

    // Build up pieces of query and result array
    $changedFields = array();
    $assignments = array();
    foreach ($this->dirtyFields as $name) {
      // Determine the type of the field
      switch ($name) {
        case 'active':
          $type = 'boolean';
          break;

        case 'x':
          $type = 'integer';
          break;

        default:
          $type = 'text';
          break;
      }

      // Determine value to write to database
      switch ($name) {
        case 'x':
          $value = floor($this->x * 1000);
          break;

        default:
          $value = $this->$name;
          break;
      }

      // Build up query pieces
      array_push($assignments,
        $conn->quoteIdentifier($name) . ' = ' . $conn->quote($value, $type));

      // Build up result
      $changedFields[$name] = $this->$name;
    }

    // Run query
    $query = 'UPDATE characters SET '
      . implode(', ', $assignments)
      . ' WHERE charId = ' . $conn->quote($this->charId, 'integer');
    $res =& $conn->exec($query);
    checkDbResult($res);
    $conn->disconnect();

    // Reset dirty fields
    $this->dirtyFields = array();

    return $changedFields;
  }

  public function asArray() {
    $keys = array_merge(array_keys($this->fields),
                        self::$virtualFields,
                        self::$readOnlyFields);

    // Fields we don't want to export
    $keysToDrop = array('wall');
    
    $result = array();
    foreach ($keys as $key) {
      if (!in_array($key, $keysToDrop)) {
        $result[$key] = $this->$key;
      }
    }
    return $result;
  }

  public function canAdminister() {
    return $this->wall->canAdminister();
  }

  public static function getCharactersPath() {
    global $config;
    return trimTrailingSlash($config['characters']['path']);
  }

  public static function getCharactersFilePath() {
    // The character path is a relative to the public folder
    return realpath(dirname(__FILE__) . '/../public/' .
                    self::getCharactersPath());
  }

  public static function getFileForId($charId) {
    return self::getCharactersFilePath() . '/' . $charId . '.svg';
  }

  public static function getRawUrl($charId) {
    return fileToUrl(self::getCharactersFilePath()) . '/' . $charId . '.svg';
  }

  public static function getGalleryUrl($charId) {
    // NOTE: When changing this it is necessary to blow away all the stored 
    // short gallery URLs since they will point to the old value for this 
    // property.
    return fileToUrl(self::getCharactersFilePath()) . '/' . $charId;
  }

  public static function getShortGalleryUrl($charId) {
    $long  = self::getGalleryUrl($charId);
    $short = shortenUrl($long);
    return $long == $short ? null : $short;
  }

  public static function getPreviewUrl($charId) {
    return fileToUrl(self::getCharactersFilePath()) . '/' . $charId
      . '/preview.svg';
  }

  public static function getPreviewFile($charId) {
    return self::getCharactersFilePath() . '/' . $charId . '.preview.svg';
  }

  public static function getEmailUrl($charId) {
    return getCurrentServer() . '/api/characters/' . $charId . '/email';
  }

  public static function sanitizeWidthOrHeight($dimension) {
    if (!is_numeric($dimension))
      throw new KeyedException('bad-request', 'Invalid width/height');

    $dimension = floatval($dimension);

    if ($dimension <= 0 || $dimension >= 10000)
      throw new KeyedException('bad-request', 'Invalid width/height');

    return $dimension;
  }

  public static function sanitizeX($x) {
    if (!is_numeric($x))
      throw new KeyedException('bad-request');

    $x = floatval($x);

    if ($x < 0 || $x > 1)
      throw new KeyedException('bad-request');

    return $x;
  }
}

abstract class CharacterDeleteMode {
  // Deletes a character's database record and file (default)
  const DeleteAll = 0;
  // Deletes a character's database record but leaves the file
  const DeleteRecordOnly = 1;
}

class Characters {
  public static function create($svg, $fields, $wallId) {
    global $config;

    // Get wall
    $wall = Walls::getById($wallId);
    if ($wall === null) {
      error_log("Wall $wallId not found when adding character");
      throw new KeyedException('wall-not-found', 'Wall not found');
    }

    // Get latest session
    $session = $wall->latestSession;
    if ($session === null || $session['end'] !== null) {
      throw new KeyedException('no-active-session');
    }

    // Check SVG
    if (isset($config['characters']['max_size']) &&
        strlen($svg) > $config['characters']['max_size']) {
      throw new KeyedException('character-too-large');
    }

    // Prepare fields
    foreach (array('title', 'author') as $field) {
      $fields[$field] = isset($fields[$field])
                        ? wtrim($fields[$field])
                        : null;
    }
    $fields['groundOffset'] = isset($fields['groundOffset'])
                      ? max(0.0, min(1.0, floatval(@$fields['groundOffset'])))
                      : 0.0;
    $fields['width']  = Character::sanitizeWidthOrHeight(@$fields['width']);
    $fields['height'] = Character::sanitizeWidthOrHeight(@$fields['height']);
    $date = gmdate('Y-m-d H:i:s');
    $x = floor($wall->getCurrentProgress() * 1000);

    // Start transaction
    $conn =& getDbConnection();
    $trans = $conn->beginTransaction();
    checkDbResult($trans);

    // Insert record
    $query =
      'INSERT INTO characters'
      . ' (sessionId, wallId, title, author, x, groundOffset, width, height,'
      . '  createDate, active)'
      . ' VALUES '
      . '(' . $conn->quote($session['sessionId'], 'integer')
      . ',' . $conn->quote($wallId, 'integer')
      . ',' . $conn->quote($fields['title'], 'text')
      . ',' . $conn->quote($fields['author'], 'text')
      . ',' . $conn->quote($x, 'integer')
      . ',' . $conn->quote($fields['groundOffset'], 'decimal')
      . ',' . $conn->quote($fields['width'], 'float')
      . ',' . $conn->quote($fields['height'], 'float')
      . ',' . $conn->quote($date, 'timestamp')
      . ',TRUE' // active 
      . ')';
    $res =& $conn->exec($query);
    checkDbResult($res, $conn);

    // Get id
    $charId = $conn->lastInsertID('characters', 'charId');
    checkDbResult($charId, $conn);

    // Save file
    $svgfilename = Character::getFileForId($charId);
    if (file_exists($svgfilename)) {
      $conn->rollback();
      error_log("SVG file already exists: $svgfilename");
      throw new KeyedException('save-failed', "Couldn't save character");
    }
    $svgfile = @fopen($svgfilename, 'w');
    if ($svgfile == false) {
      $conn->rollback();
      error_log("Failed to save SVG to file: $svgfilename");
      throw new KeyedException('save-failed', "Couldn't save character");
    }
    fwrite($svgfile, $svg);
    fclose($svgfile);

    // Commit transaction
    $conn->commit();

    // Store shortened URL for future reference
    $char = self::getById($charId);
    if ($char->galleryUrlShort) {
      $char->save();
    }

    CharacterMakeAPI::create($char->galleryUrl, $char->rawUrl, $char->title, $char->author, $char->wallId, $char->sessionId);
    // Return char
    return $char;
  }

  public static function getById($charId) {
    // Prepare parameters
    $charId = intval($charId);
    if ($charId < 1)
      throw new KeyedException('bad-request');

    // Run query
    $conn =& getDbConnection();
    $row =& $conn->queryRow(
        'SELECT characters.* FROM characters'
      . ' WHERE characters.charId = ' . $conn->quote($charId, 'integer')
      . ' LIMIT 1',
      null,
      MDB2_FETCHMODE_ASSOC
    );
    $conn->disconnect();
    checkDbResult($row);

    // Check if character was found
    if ($row === null)
      return null;

    return self::dbRowToChar($row);
  }

  private static function dbRowToChar($row) {
    // Map database fields
    $data = array();
    $mapping = array('sessionid' => 'sessionId',
                     'wallid' => 'wallId',
                     'title',
                     'author',
                     'x',
                     'groundoffset' => 'groundOffset',
                     'width',
                     'height',
                     'galleryurlshort' => 'galleryUrlShort',
                     'createdate' => 'createDate',
                     'active' => 'active');
    foreach ($mapping as $dbKey => $mdKey) {
      $dbKey = is_int($dbKey) ? $mdKey : $dbKey;
      if (!array_key_exists($dbKey, $row)) {
        error_log("Failed to find field '$dbKey' when loading character");
        throw new KeyedException('db-error');
      }
      $data[$mdKey] = $row[$dbKey];
    }

    // Process values that need special handling
    $data['active']       = (bool)$data['active'];
    $data['wallId']       = intval($data['wallId']);
    $data['sessionId']    = intval($data['sessionId']);
    $data['x']            = intval($data['x']) / 1000;
    $data['groundOffset'] = floatval($data['groundOffset']);
    $data['width']        = floatval($data['width']);
    $data['height']       = floatval($data['height']);

    // Clear the shortened URL field if it is not set so we can try to fill it 
    // in later
    if ($data['galleryUrlShort'] === null) {
      unset($data['galleryUrlShort']);
    }

    // Create character
    return new Character(intval($row['charid']), $data);
  }

  public static function getBySession($wallId, $sessionId) {
    // Prepare parameters
    $wallId = @intval($wallId);
    if ($wallId < 1)
      throw new KeyedException('bad-request');
    $sessionId = @intval($sessionId);
    if ($sessionId < 1)
      throw new KeyedException('bad-request');

    // Verify IDs are valid
    // (We return an empty array if we have valid IDs but no characters but
    // return NULL if the IDs are not valid. This is consistent with 
    // Characters::getById etc. where 'not found' is represented by null)
    if (!self::isValidSession($wallId, $sessionId))
      return null;

    // Run query
    $conn =& getDbConnection();
    $res =& $conn->query(
        'SELECT * FROM characters'
      . ' WHERE wallId = ' . $conn->quote($wallId, 'integer')
      . ' AND   sessionId = ' . $conn->quote($sessionId, 'integer')
      . ' ORDER BY charId');
    $conn->disconnect();
    checkDbResult($res);

    // Prepare result
    $result = array();
    $conn->setFetchMode(MDB2_FETCHMODE_ASSOC);
    while ($row = $res->fetchRow()) {
      array_push($result, self::dbRowToChar($row));
    }

    return $result;
  }

  public static function getByWall($wallId) {
    // Prepare parameters
    $wallId = intval($wallId);
    if ($wallId < 1)
      throw new KeyedException('bad-request');

    // Verify wall ID is valid
    $wall = Walls::getById($wallId);
    if (!$wall)
      return null;

    // Run query
    $conn =& getDbConnection();
    $res =& $conn->query(
        'SELECT * FROM characters'
      . ' WHERE wallId = ' . $conn->quote($wallId, 'integer')
      . ' ORDER BY sessionId, charId');
    $conn->disconnect();
    checkDbResult($res);

    // Prepare result
    $result    = array();
    $session   = array();
    $sessionId = null;
    $conn->setFetchMode(MDB2_FETCHMODE_ASSOC);
    while ($row = $res->fetchRow()) {
      if ($sessionId && $sessionId != $row['sessionid']) {
        $result[$sessionId] = $session;
        $session = array();
      }
      array_push($session, self::dbRowToChar($row));
      $sessionId = $row['sessionid'];
    }
    if ($sessionId) {
      $result[$sessionId] = $session;
    }

    return $result;
  }

  public static function deleteById($charId,
    $deleteMode = CharacterDeleteMode::DeleteAll)
  {
    // Prepare parameters
    $charId = intval($charId);
    if ($charId < 1)
      throw new KeyedException('bad-request', "Bad character ID");

    // Check for permission
    $char = self::getById($charId);
    if (!$char)
      return null;
    if (!$char->canAdminister())
      throw new KeyedException('no-auth');

    // Start transaction
    $conn =& getDbConnection();
    $trans = $conn->beginTransaction();
    checkDbResult($trans);

    // Delete record
    $res =& $conn->exec('DELETE FROM characters WHERE charId = '
                        . $conn->quote($charId, 'integer'));
    checkDbResult($res, $conn);

    // If no rows were affected this character has already been deleted
    if ($res === 0) {
      $conn->rollback();
      return false;
    }

    // Delete corresponding SVG file
    if ($deleteMode === CharacterDeleteMode::DeleteAll) {
      $svgfilename = Character::getFileForId($charId);
      $svgFolder = dirname($svgfilename);
      if (file_exists($svgfilename)) {
        if (@unlink($svgfilename) === FALSE) {
          $conn->rollback();
          error_log("Failed to delete file: $svgfilename");
          throw new KeyedException('server-error', "Failed to delete file");
        }
      }

      // Delete preview file if it exists
      $preview = Character::getPreviewFile($charId);
      if (file_exists($preview) && @unlink($preview) === FALSE) {
        // We don't rollback in this case partly because it's not a big deal 
        // if we can't clear up the previews---that could could handled by 
        // a cron job if necessary---and partly because it would be quite 
        // complicated to also restore the SVG file in that case.
        error_log("Failed to delete preview file: $preview");
      }
    }

    // Commit transaction
    $conn->commit();

    CharacterMakeAPI::remove($char->galleryUrl);
    return true;
  }

  public static function deleteBySession($wallId, $sessionId,
    $deleteMode = CharacterDeleteMode::DeleteAll)
  {
    // _bulkDelete permits a null sessionId (it means, "all sessions for this 
    // wall"), but this method should be getting called with a null value so we 
    // handle it here instead
    if ($sessionId === null)
      throw new KeyedException('bad-request');

    return self::_bulkDelete($wallId, $sessionId, $deleteMode);
  }

  public static function deleteByWall($wallId,
    $deleteMode = CharacterDeleteMode::DeleteAll)
  {
    return self::_bulkDelete($wallId, null, $deleteMode);
  }

  protected static function _bulkDelete($wallId, $sessionId, $deleteMode) {
    // Prepare parameters
    $wallId = intval($wallId);
    if ($wallId < 1)
      throw new KeyedException('bad-request');
    $sessionId = toIntOrNull($sessionId);
    if ($sessionId !== null && $sessionId < 1)
      throw new KeyedException('bad-request');

    // Verify IDs are valid
    // (We return zero if we have valid IDs but no characters but
    // return NULL if the IDs are not valid.)
    if ($sessionId) {
      if (!self::isValidSession($wallId, $sessionId))
        return null;
    } else {
      $wall = Walls::getById($wallId);
      if (!$wall)
        return null;
    }

    // Set up condition for selecting characters
    $conn =& getDbConnection();
    $cond = 'wallId = ' . $conn->quote($wallId, 'integer');
    if ($sessionId) {
      $cond .= ' AND sessionId = ' . $conn->quote($sessionId, 'integer');
    }

    // Get list of characters to remove
    // (We need this so we can delete the corresponding files)
    $charIds =& $conn->queryCol('SELECT charId FROM characters WHERE ' . $cond,
                                'integer');
    checkDbResult($charIds);

    // Check if this session/wall is already empty
    if (count($charIds) === 0)
      return 0;

    // Start transaction
    $trans = $conn->beginTransaction();
    checkDbResult($trans);

    // Delete records
    $res =& $conn->exec('DELETE FROM characters WHERE ' . $cond);
    checkDbResult($res, $conn);
    $rowsAffected = $res;

    // Delete corresponding SVG files
    if ($deleteMode === CharacterDeleteMode::DeleteAll) {
      $deleter = new BatchFileDeleter();
      foreach ($charIds as $charId) {
        $filename = Character::getFileForId($charId);
        if (!$deleter->deleteFile($filename)) {
          $deleter->rollback();
          $conn->rollback();
          error_log("Failed to delete file: $filename");
          throw new KeyedException('server-error', "Failed to delete file");
        }

        // Delete the preview file too.
        //
        // If this fails we don't care. And if we have a failure later on, 
        // there's no need to restore these files since they will be 
        // auto-generated anyway.
        $preview  = Character::getPreviewFile($charId);
        @unlink($preview);
      }
      $deleter->commit();
    }

    // Commit transaction
    $conn->commit();

    return $rowsAffected;
  }

  private static function isValidSession($wallId, $sessionId) {
    $conn =& getDbConnection();
    $res =& $conn->query(
           'SELECT sessionId FROM sessions '
           . ' WHERE wallId = ' . $conn->quote($wallId, 'integer')
           . ' AND sessionId = ' . $conn->quote($sessionId, 'integer')
           . ' LIMIT 1');
    checkDbResult($res);
    $validSession = $res->numRows() !== 0;
    $res->free();
    return $validSession;
  }
}

// A helper class that deletes a series of files in such a way that the whole 
// lot can be rolled back
class BatchFileDeleter {
  // An array that maps each file that has been deleted to the temporary 
  // location where it is stored
  private $movedFiles = array();

  public function __destruct() {
    $this->rollback();
  }

  public function deleteFile($file) {
    // If there is no file, fail silently
    if (!file_exists($file))
      return true;

    // Make place to move file to
    $temp = tempnam(sys_get_temp_dir(), 'character');
    if ($temp === FALSE)
      throw new KeyedException('server-error',
                               "Couldn't create a temporary file");

    // On Windows, rename fails if the target exists so delete it
    unlink($temp);

    // Move file
    if (@rename($file, $temp) === FALSE)
      return false;

    // Store action so we can rollback / commit
    $this->movedFiles[$file] = $temp;

    return true;
  }

  public function commit() {
    // Remove temporary files
    foreach($this->movedFiles as $tempFile) {
      // It doesn't really matter if this fails since it's a temporary file and 
      // will get cleaned up eventually anyway
      unlink($tempFile);
    }

    // Empty list of files
    $this->movedFiles = array();
  }

  public function rollback() {
    $failedRollback = false;
    foreach($this->movedFiles as $origPath => $movedPath) {
      // Revert each action
      if (rename($movedPath, $origPath) === FALSE) {
        // If we failed to restore a file we're really unlucky.
        //
        // This whole class is built around the assumption that if we can move 
        // something to a temporary folder we can also move it back.
        //
        // For now just keep on going. We may be able to restore SOME files 
        // which would be better in terms of avoiding data loss.
        // We'll throw an exception when we're done.
        $failedRollback = true;
      }

      // Empty list of files
      $this->movedFiles = array();
    }
    if ($failedRollback)
      throw new KeyedException('server-error',
                               "Failed to restore deleted files");
  }
}

?>
