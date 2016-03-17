<?php

/**
 * A migration of CSN content from a CSV
 */
class CSNContent extends Migration {
  /**
   * Define migration
   */
  public function __construct($arguments) {
    parent::__construct($arguments);
    $this->description = t('Import CSN content from CSV');

    // Source
    $path = DRUPAL_ROOT . '/sites/all/modules/custom/csnedu_migrate/content.csv';
    $columns = array(
      array('title', 'title'),
      array('pagetitle', 'pagetitle'),
      array('id', 'id'),
      array('url', 'url'),
      array('keywords', 'keywords'),
      array('description', 'description'),
      array('content', 'content'),
      array('left', 'left'),
      array('right', 'right'),
      array('alias', 'alias'),
    );
    $options = array(
      'delimiter' => ',',
      'header_rows' => 1,
    );
    $this->source = new MigrateSourceCSV($path, $columns, $options);

    // Map
    $this->map = new MigrateSQLMap($this->machineName, array(
      'id' => array(
        'type' => 'int',
        'length' => 8,
        'not null' => TRUE,
        'description' => 'Content ID',
      ),
    ), MigrateDestinationNode::getKeySchema());

    // Destination
    $this->destination = new MigrateDestinationNode('page');
    $this->addFieldMapping('title', 'title');
    $this->addFieldMapping('title_field', 'title');
    $this->addFieldMapping('body', 'content');
    $this->addFieldMapping('body:format')
      ->defaultValue('full_html');
    $this->addFieldMapping('field_old_url', 'url');
    $this->addFieldMapping('field_flag', 'flag');
    $this->addFieldMapping('metatag_keywords', 'keywords');
    $this->addFieldMapping('metatag_description', 'description');
    $this->addFieldMapping('metatag_title', 'pagetitle');
    $this->addFieldMapping('field_body_image', 'images');
    $this->addFieldMapping('field_body_image:file_class')
     ->defaultValue('MigrateFileFid');
    $this->addFieldMapping('field_files', 'files');
    $this->addFieldMapping('field_files:file_class')
     ->defaultValue('MigrateFileFid');
    $this->addFieldMapping('field_issues', 'issues');
    $this->addFieldMapping('log')
      ->defaultValue('Automated Migration');
    $this->addFieldMapping('status')
      ->defaultValue(1);
    $this->addFieldMapping('uid')
      ->defaultValue(1);
    $this->addFieldMapping('pathauto')
      ->defaultValue(1);
  }

  /**
   * Clean up row content
   */
  public function prepareRow($row) {
    if (parent::prepareRow($row) === FALSE) {
      return FALSE;
    }

    $row->title = trim($row->title);
    if (empty($row->title)) {
      $this->queueMessage('Empty Title');
      return FALSE;
    }

    // load existing node if present
    $destid = $this->getMap()->lookupDestinationID(array('id' => $row->id));
    $node = new stdClass();
    if (!empty($destid['destid1'])) {
      $node = node_load($destid['destid1']);
    }

    // prevent saving if checked that it is cleaned
    if (!empty($node->field_cleaned[LANGUAGE_NONE][0]['value'])) {
      return FALSE;
    }

    // keep flagged if flagged
    $row->flag = 0;
    if (!empty($node->field_flag[LANGUAGE_NONE][0]['value'])) {
      $row->flag = 1;
    }

    // keep existing issues
    $row->issues = array();
    if (!empty($node->field_issues[LANGUAGE_NONE])) {
      foreach ($node->field_issues[LANGUAGE_NONE] as $issue) {
        $row->issues[] = $issue['value'];
      }
    }

    // fetch content from live site if we want to do scraping
    if (empty($row->content) && !empty($row->url)) {
      $result = drupal_http_request($row->url);
      if (!empty($result->data) && $result->code == 200) {
        // pull in only body content
        $doc = new DOMDocument('1.0', 'utf-8');
        libxml_use_internal_errors(true);
        $doc->preserveWhiteSpace = FALSE;
        $doc->strictErrorChecking = FALSE;
        $doc->substituteEntities = FALSE;
        $doc->loadHTML($result->data);
        $xpath = new DOMXpath($doc);

        // pull out content
        $content = $xpath->query("//span[@id='ContentBlock1']");
        $entry = $content->item(0);
        if (empty($entry)) {
          return FALSE;
        }
        $row->content = (string) $entry->ownerDocument->saveHTML($entry);

        // remove stuff that's hard to get rid of
        $row->content = preg_replace('/.*<!-- End Main PreContent Area -->/s', '', $row->content);
        $row->content = preg_replace('/<!-- Begin Main PostContent Area -->.*/s', '', $row->content);
      }
      if (empty($row->content)) {
        return FALSE;
      }
    }

    // look for sidebars
    if (!empty($row->right)) {
      $row->issues[] = t('Right Sidebar content');
    }
    if (!empty($row->left)) {
      $row->issues[] = t('Left Sidebar content');
    }

    // look for empty content
    $row->content = trim($row->content);
    if (empty($row->content)) {
      $row->flag = 1;
      $row->issues[] = t('No content');

      // the rest is content cleanup so stop
      return TRUE;
    }

    // clean up includes
    $count = 0;
    $row->content = preg_replace('/(<%.+?%>)/s', '', $row->content, -1, $count);
    if ($count > 0) {
      $row->issues[] = t('ASP Includes');
    }

    $count = 0;
    $row->content = preg_replace('/(<!--#include .+? -->)/s', '', $row->content, -1, $count);
    if ($count > 0) {
      $row->issues[] = t('Server Includes');
    }

    // clean up scripts
    $count = 0;
    $row->content = preg_replace('/(<script.*<\/script>)/s', '', $row->content, -1, $count);
    if ($count > 0) {
      $row->issues[] = t('Scripts');
    }

    // look for forms
    $count = preg_match_all('/<form(.*)<\/form>/s', $row->content);
    if ($count > 0) {
      $row->issues[] = t('Forms');
    }

    // look for iframes
    $count = preg_match_all('/<iframe(.*)>/s', $row->content);
    if ($count > 0) {
      $row->issues[] = t('iframes');
    }

    // look for flash
    $count = preg_match_all('/<object(.*)>/s', $row->content);
    if ($count > 0) {
      $row->issues[] = t('Flash');
    }

    // clean up microsoft word formatting and validate / tidy HTML
    $config = array(
      'indent' => FALSE,
      'output-xhtml' => TRUE,
      'wrap' => 2000,
      'clean' => TRUE,
      'bare' => TRUE,
      'hide-comments' => TRUE,
      'word-2000' => TRUE,
      'show-warnings' => FALSE,
      'char-encoding' => 'utf8',
      'input-encoding' => 'utf8',
      'output-encoding' => 'utf8',
      'ascii-chars' => FALSE,
    );
    $tidy = tidy_parse_string($row->content, $config, 'UTF8');
    $tidy->cleanRepair();
    $tidy->diagnose();
    if (tidy_error_count($tidy) > 0) {
      $errors = explode("\n", $tidy->errorBuffer);
      foreach ($errors as $error) {
        $row->issues[] = $error;
      }
      // get rid of last status message
      array_pop($row->issues);
    }
    $html = $tidy->html();
    $html = mb_convert_encoding($html->value, 'HTML-ENTITIES', 'UTF-8');

    // load into xpath
    $doc = new DOMDocument('1.0', 'utf-8');
    libxml_use_internal_errors(true);
    $doc->preserveWhiteSpace = FALSE;
    $doc->strictErrorChecking = FALSE;
    $doc->substituteEntities = FALSE;
    $doc->loadHTML($html);
    $xpath = new DOMXpath($doc);

    // parse all links
    $links = $xpath->query('//a');
    foreach ($links as $link) {
      $href = $link->getAttribute('href');
      $url = parse_url($href);

      if (!empty($url['scheme']) && !in_array($url['scheme'], array('http', 'https'))) {
        // skip non http links (mailto, javascript, etc)
        continue;
      }

      // skip empty paths
      if (empty($url['path'])) {
        continue;
      }

      if (isset($url['host']) && $url['host'] == 'csn.edu' || $url['host'] == 'www.csn.edu') {
        // update local non-relative urls to be relative if local
        $path = trim($url['path'], '/');
        if (!empty($url['query'])) {
          $path .= '?' . $url['query'];
        }
        if (!empty($url['fragment'])) {
          $path .= '#' . $url['fragment'];
        }
        $link->setAttribute('href', '/' . $path);

        // update url data
        $url = parse_url($path);
      }
      elseif (isset($url['host'])) {
        // not relative
        continue;
      }

      // replace link if it goes to an existing redirect
      $path = trim($url['path'], '/');
      $existing = redirect_load_by_source($path);
      if (!empty($existing->redirect)) {
        $path = trim($existing->redirect, '/');
        if (!empty($url['query'])) {
          $path .= '?' . $url['query'];
        }
        if (!empty($url['fragment'])) {
          $path .= '#' . $url['fragment'];
        }
        $link->setAttribute('href', '/' . $path);

        // update url data
        $url = parse_url($path);
      }

      // check path extension
      $path = trim($url['path'], '/');
      $path = pathinfo($path);
      if (empty($path['extension'])) {
        continue;
      }

      // check that it is allowed
      $allowed_extensions = array(
        'pdf',
        'docx',
        'PDF',
        'xlsx',
        'csv',
        'doc',
        'rtf',
        'txt',
        'ppt',
        'mp3',
        'pps',
        'jpg',
        'png',
        'gif',
        'xls',
        'jpeg',
        'pptx',
        'tar',
        'JPG',
        'xml',
        'ppsx',
        'psd',
        'xlsm',
        'ram',
        'asx',
      );
      if (!in_array($path['extension'], $allowed_extensions)) {
        $known_extensions = array(
          'asp',
          'aspx',
          'as',
          'htm',
          'html',
          'StudentRecruitment',
        );
        if (!in_array($path['extension'], $known_extensions)) {
          $this->queueMessage('new extension: ' . $path['extension']);
        }
        continue;
      }

      // if already local, load the file
      $path = trim($url['path'], '/');
      $path = str_replace('sites/default/files/', '', $path);
      $uri = file_build_uri($path);
      $files = file_load_multiple(array(), array('uri' => $uri));
      $file = reset($files);
      if (!empty($file->fid)) {
        $row->files[] = $file->fid;
        $url = file_create_url($file->uri);
        $href = parse_url($url);
        $path = trim($href['path'], '/');
        $link->setAttribute('href', '/' . $path);
        continue;
      }

      // save file locally
      $path = trim($url['path'], '/');
      if (!empty($url['query'])) {
        $path .= '?' . $url['query'];
      }
      if (!empty($url['fragment'])) {
        $path .= '#' . $url['fragment'];
      }
      $file = $this->save_file($path, 'public://documents/imported');
      if (!empty($file->fid)) {
        $row->files[] = $file->fid;

        if (!empty($file->uri)) {
          // replace link to file
          $url = file_create_url($file->uri);
          $href = parse_url($url);
          $path = trim($href['path'], '/');
          $link->setAttribute('href', '/' . $path);
        }
      }
      else {
        // save file failed, probable bad link
        $row->issues[] = t('Possible Bad Link: ' . $path);
      }
    }

    // clean up images, add redirects
    $images = $xpath->query("//img");
    foreach ($images as $image) {
      $src = $image->getAttribute('src');
      $url = parse_url($src);

      // skip empty paths
      if (empty($url['path'])) {
        continue;
      }

      // update local non-relative urls to be relative
      if (isset($url['host']) && $url['host'] == 'csn.edu' || $url['host'] == 'www.csn.edu') {
        // update local non-relative urls to be relative if local
        $path = trim($url['path'], '/');
        if (!empty($url['query'])) {
          $path .= '?' . $url['query'];
        }
        if (!empty($url['fragment'])) {
          $path .= '#' . $url['fragment'];
        }
        $image->setAttribute('src', '/' . $path);

        // update url data
        $url = parse_url($path);
      }
      elseif (isset($url['host'])) {
        // not relative
        continue;
      }

      // replace link if it goes to an existing redirect
      $path = trim($url['path'], '/');
      $existing = redirect_load_by_source($path);
      if (!empty($existing->redirect)) {
        $path = trim($existing->redirect, '/');
        if (!empty($url['query'])) {
          $path .= '?' . $url['query'];
        }
        if (!empty($url['fragment'])) {
          $path .= '#' . $url['fragment'];
        }
        $image->setAttribute('src', '/' . $path);

        // update url data
        $url = parse_url($path);
      }

      // if already local, load the file
      $path = trim($url['path'], '/');
      $path = str_replace('sites/default/files/', '', $path);
      $uri = file_build_uri($path);
      $files = file_load_multiple(array(), array('uri' => $uri));
      $file = reset($files);
      if (!empty($file->fid)) {
        $row->images[] = $file->fid;
        $url = file_create_url($file->uri);
        $href = parse_url($url);
        $path = trim($href['path'], '/');
        $image->setAttribute('src', '/' . $path);
        continue;
      }

      // save image locally
      $path = trim($url['path'], '/');
      if (!empty($url['query'])) {
        $path .= '?' . $url['query'];
      }
      if (!empty($url['fragment'])) {
        $path .= '#' . $url['fragment'];
      }
      $file = $this->save_file($path, 'public://images/imported');
      if (!empty($file->fid)) {
        $row->images[] = $file->fid;

        if (!empty($file->uri)) {
          // replace link to image
          $url = file_create_url($file->uri);
          $href = parse_url($url);
          $path = trim($href['path'], '/');
          $image->setAttribute('src', '/' . $path);
        }
      }
      else {
        // save file failed, probable bad link
        $row->issues[] = t('Possible Bad Link: ' . $path);
      }
    }

    // get only the body output
    $html = '';
    $children = $doc->getElementsByTagName('body')->item(0)->childNodes;
    foreach ($children as $child) {
      $html .= $doc->saveHTML($child);
    }
    $html = mb_convert_encoding($html, 'HTML-ENTITIES', 'UTF-8');
    $row->content = $html;

    // remove duplicate and not good issues
    $skip = array(
      'Info: Document content looks like HTML Proprietary',
      'Info: Document content looks like XHTML 1.0 Transitional',
    );
    if (!empty($row->issues)) {
      $row->issues = array_unique($row->issues);

      foreach ($row->issues as $delta => $issue) {
        if (strpos($issue, 'st1:') !== FALSE) {
          unset($row->issues[$delta]);
        }
        if (strpos($issue, 'Not all warnings/errors were shown.') !== FALSE) {
          unset($row->issues[$delta]);
        }
        if (in_array($issue, $skip)) {
          unset($row->issues[$delta]);
        }
      }
    }

    // set flag if we ran into issues
    if (!empty($row->issues)) {
      $row->flag = 1;
    }
  }

  /**
   * Run once done creating node
   */
  public function complete($entity, $row) {
    // add redirect for old page URL
    if (!empty($row->url) && !empty($entity->nid)) {
      $url = parse_url($row->url);
      $path = trim($url['path'], '/');
      if (!empty($url['query'])) {
        $path .= '?' . $url['query'];
      }
      if (!empty($url['fragment'])) {
        $path .= '#' . $url['fragment'];
      }
      $this->add_redirect($path, 'node/' . $entity->nid);
    }

    // add redirect for any aliases
    if (!empty($row->alias) && !empty($entity->nid)) {
      $path = trim($row->alias, '/');
      $this->add_redirect($path, 'node/' . $entity->nid);
    }
  }

  /**
   * Custom fields
   */
  public function fields() {
    return array(
      'flag' => 'Whether to flag this content or not',
      'issues' => 'Issue descriptions',
      'images' => 'Images Added',
      'files' => 'Files Added',
    );
  }

  /**
   * Fetch a file and save to files directory in given directory
   * Stores in the file table as permanent so we can see later
   * Also adds a redirect
   * Assumes the input is relative to a base url
   */
  private function save_file($source, $directory) {
    $result = drupal_http_request('http://www.csn.edu/' . $source);
    if ($result->code != 200 || empty($result->data)) {
      return FALSE;
    }

    // get the filename to save to
    $path = pathinfo($source);
    if (empty($path['basename'])) {
      return FALSE;
    }
    $file_name = urldecode($path['basename']);
    $file_name = transliteration_clean_filename($file_name);

    // make sure destination is setup
    $destination = $directory . '/' . $file_name;
    file_prepare_directory($directory, FILE_CREATE_DIRECTORY | FILE_MODIFY_PERMISSIONS);

    // save the file
    $file = file_save_data($result->data, $destination, FILE_EXISTS_REPLACE);
    if (empty($file->uri)) {
      return FALSE;
    }

    // add a redirect
    $destination = file_create_url($file->uri);
    $this->add_redirect($source, $destination);

    return $file;
  }

  /**
   * Add a redirect
   * Assumes source is relative
   */
  private function add_redirect($source_url, $destination_url) {
    $redirect = array();

    // parse source
    $source = redirect_parse_url($source_url);
    $redirect['source'] = $source['url'];
    if (!empty($source['query'])) {
      $redirect['source_options']['query'] = $source['query'];
    }

    // check if exists
    if (empty($redirect['source_options']['query'])) {
      $existing = redirect_load_by_source($redirect['source']);
      if (!empty($existing)) {
        return FALSE;
      }
    }
    else {
      $existing = redirect_load_by_source($redirect['source'], LANGUAGE_NONE, $redirect['source_options']['query']);
      if (!empty($existing)) {
        return FALSE;
      }
    }

    // parse destination
    $destination = redirect_parse_url($destination_url);
    $redirect['redirect'] = $destination['url'];
    if (isset($destination['query'])) {
      $redirect['redirect_options']['query'] = $destination['query'];
    }
    if (isset($destination['fragment'])) {
      $redirect['redirect_options']['fragment'] = $destination['fragment'];
    }

    // save redirect
    $redirect_object = new stdClass();
    redirect_object_prepare($redirect_object, $redirect);
    redirect_save($redirect_object);

    return $redirect_object;
  }
}