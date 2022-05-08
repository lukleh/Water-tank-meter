$(function() {
  console.log("ready!");
  var dataInterval = 60 * 1000;

  function alert(message, type) {
		$("#liveAlertPlaceholder").empty();
    var wrapper = document.createElement('div')
    wrapper.innerHTML = '<div class="alert alert-' + type + ' alert-dismissible fade show" role="alert">' + message + '<button type="button" class="btn-close" data-bs-dismiss="alert" aria-label="Close"></button></div>'
    $("#liveAlertPlaceholder").append(wrapper)
  }

  $("#savesetup").on("click", function(e) {

    var form = $("#setup");
    var actionUrl = form.attr('action');
    console.log(form.serialize());
    $.ajax({
      type: "POST",
      url: "/save",
      data: form.serialize(),
      success: function(data) {
        $("p.setup-result").html(JSON.stringify(data));
      }
    }).done(function(json) {
      alert('Sensor setup saved.', 'success')
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
		if (!json.info["wifi name"] || !json.info["wifi password"] || json.info["sensor height"] <= 0 || json.info["tank diameter"] <= 0) {
			$("#setupModal").modal('show');
			return;
		}
    if (!$('#setup input').is(":focus")) {
      $("#wifi_ssid").val(json.info["wifi name"]);
      $("#wifi_password").val(json.info["wifi password"]);
      $("#sensor_height_cm").val(json.info["sensor height"]);
      $("#tank_diameter_cm").val(json.info["tank diameter"]);
    }
  }

  function updateTank(json) {
    $("#tanktext").text(json.info["percent full"] + "%");
		$("#tanktextVol").html(json.info["volume"].toFixed(2) + "m<sup>3</sup>");
    $("div.progress-fill").css("height", json.info["percent full"] + "%");
  }

  function updateHistory(json) {
    $("#historyTable tr").slice(1).remove();
    for (const element of json.data) {
			$("#historyTable").append(`<tr><td>${element.time}</td><td>${element.distance}cm</td><td>${element.volume.toFixed(2)}m<sup>3</sup></td></tr>`);
    }
  }

	function updateInfo(json) {
    $("#infoTable tr").remove();
    for (var key in json.info) {
      $("#infoTable").append(`<tr><td>${key}</td><td>${json.info[key]}</td></tr>`);
    }
    rssi = json.info["signal strength dBi"]
    rssi_percent = Math.min(Math.max(2 * (rssi + 100), 0), 100)
    $("#wifistrength").css("width", rssi_percent + "%").attr("aria-valuenow", rssi_percent).text("WIFI " + rssi_percent + "%");
  }

  function refreshData() {
    $.ajax({
      url: "/data",
      type: "GET",
      dataType: "json",
      linktimeout: 2000
    }).done(function(json) {
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

})
