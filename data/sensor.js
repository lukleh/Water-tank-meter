$(function() {
  console.log("ready!");
  var dataInterval = 60 * 1000;
  var timer = Date.now();
  function alert(message, type) {
		$("#liveAlertPlaceholder").empty();
    var wrapper = document.createElement('div')
    wrapper.innerHTML = '<div class="alert alert-' + type + ' alert-dismissible fade show" role="alert">' + message + '<button type="button" class="btn-close" data-bs-dismiss="alert" aria-label="Close"></button></div>'
    $("#liveAlertPlaceholder").append(wrapper)
  }

  $("#savesetup").on("click", function(e) {

    var form = $("#setup");

    $.ajax({
      type: "POST",
      url: "/save",
      data: form.serialize(),
      success: function(data) {
        $("p.setup-result").html(JSON.stringify(data));
      }
    }).done(function(json) {
      alert('Sensor setup saved.', 'success');
    }).fail(function(xhr, status, errorThrown) {
      alert('Sensor setup could not save!', 'danger')
      console.log("Error: " + errorThrown);
      console.log("Status: " + status);
      console.dir(xhr);
    }).always(function() {
      console.log('Waiting ' + (dataInterval / 1000) + ' seconds');
      setTimeout(refreshData, dataInterval);
    });

		$("#setupModal").modal('hide');
  });

  function populateForm(json) {
    if (!$('#setup input').is(":focus")) {
      $("#wifi_ssid").val(json.info["wifi name"]);
      $("#wifi_password").val(json.info["wifi password"]);
      $("#sensor_name").val(json.info["sensor name"]);
      $("#sensor_distance_empty_cm").val(json.info["sensor distance empty cm"]);
      $("#tank_diameter_cm").val(json.info["tank diameter"]);
      $("#sensor_distance_full_cm").val(json.info["sensor distance full cm"]);
    }
  }

  function updateTank(json) {
    if (json.info["current distance"] == 0) {
      $("#tanktext").text("SENSOR");
      $("#tanktextVol").text("NO DATA");
      $("div.progress-fill").css("height", 0 + "%");
    } else {
      $("#tanktext").text(json.info["percent full"] + "%");
  		$("#tanktextVol").html(json.info["volume"].toFixed(2) + "m<sup>3</sup>");
      $("div.progress-fill").css("height", json.info["percent full"] + "%");
    }
  }

  function updateHistory(json) {
    $("#historyGraph div").remove();
    for (const element of json.data) {
      $("#historyGraph").append(`
        <div class="row">
          <div class="col-2 pe-0">
            <label>${element.p}%</label>
          </div>
          <div class="col-10 ps-0">
            <div class="progress" style="height: 23px;">
              <div class="progress-bar" role="progressbar" style="width: ${element.p}%;" aria-valuenow="${element.p}" aria-valuemin="0" aria-valuemax="100"></div>
            </div>
          </div>
        </div>`);
    }
    $("#historyTable tr").slice(1).remove();
   for (const element of json.data) {
     $("#historyTable").append(`<tr><td>${element.i}</td><td>${element.d}cm</td><td>${element.v}m<sup>3</sup></td></tr>`);
   }
  }

	function updateInfo(json) {
    $("#infoTable tr").remove();
    for (var key in json.info) {
      $("#infoTable").append(`<tr><td>${key}</td><td>${json.info[key]}</td></tr>`);
    }
    if ("signal strength dBi" in json.info) {
      rssi = json.info["signal strength dBi"]
      rssi_percent = Math.min(Math.max(2 * (rssi + 100), 0), 100)
      $("#wifistrengthbox").show();
      $("#wifistrength").css("width", rssi_percent + "%").attr("aria-valuenow", rssi_percent).text("WIFI " + rssi_percent + "%");
    } else {
      $("#wifistrengthbox").hide();
    }
    $("#version").text("v" + json.info["firmware version"])
  }

  function refreshTimer() {
      $("#timer").text(Math.floor(-1 * (Date.now() - timer - dataInterval) / 1000));
      setTimeout(refreshTimer, 1000);
  }

  function refreshData() {
    $.ajax({
      url: "/data",
      type: "GET",
      dataType: "json",
      linktimeout: 2000
    }).done(function(json) {
      timer = Date.now();
			if (!json || !json.info || !json.data) {
				alert('Received broken data, will retry.', 'warning')
				dataInterval = 5 * 1000;
				return
			}
      populateForm(json);
      updateTank(json);
			updateInfo(json);
      updateHistory(json);
			$("#liveAlertPlaceholder div.alert-warning").remove();
			dataInterval = 60 * 1000;
    }).fail(function(xhr, status, errorThrown) {
      timer = Date.now();
			alert('Lost connection, retrying....', 'warning')
			dataInterval = 5 * 1000;
      console.log("Error: " + errorThrown);
      console.log("Status: " + status);
      console.dir(xhr);
    }).always(function() {
      console.log('Waiting ' + (dataInterval / 1000) + ' seconds');
      setTimeout(refreshData, dataInterval);
    });
  }
  refreshData();
  refreshTimer();
})
